// SPDX-License-Identifier: GPL-2.0
#ifndef TBV_VERBS_INTERNAL_H
#define TBV_VERBS_INTERNAL_H

#include "tbv.h"
#include <linux/refcount.h>
#include <linux/spinlock.h>
#include <linux/version.h>
#include <linux/workqueue.h>
#include <rdma/ib_umem.h>
#include <rdma/ib_verbs.h>

struct tbv_state;
struct tbv_dmabuf_mr;
struct seq_file;

/*
 * Locking
 *
 * Order, outermost first. "A > B" means A is held while B is taken, at a
 * call site that was read at both ends. Take them in this order and release
 * them in the reverse.
 *
 *   ctrl->setup_lock > peer->dmabuf_control_lock > peer->control_lock
 *   peer->control_lock       > state->lock
 *   state->rail_register_lock > state->lock
 *   dmabuf_engine.rx_lock    > state->lock
 *   dmabuf_engine.rx_lock    > dmabuf_engine.ring_lock
 *   state->lock              > path->tx_lock
 *   state->lock              > dmabuf_engine.ring_lock   (trylock only)
 *   state->lock              > tqp->lock
 *   tqp->rx_lock             > tqp->lock
 *   tqp->ack_lock            > tqp->lock, send->lock, cq->lock
 *   read->data_lock          > tqp->lock
 *
 * Inferred from call chains but not confirmed at both ends, so do not rely
 * on these without re-reading them:
 *
 *   dmabuf_engine.rx_lock    > tqp->rx_lock > tqp->lock
 *   tqp->rx_lock             > cq->lock, path->tx_lock
 *   tqp->post_lock           > tqp->lock, state->lock, path->tx_lock
 *
 * What each one covers:
 *
 * state->lock       peers, rails, links, gsi_qp and path selection.
 *                   tbv_path_get_gsi_qp() takes tqp->lock under it; the
 *                   reverse order was not found.
 * rail_register_lock ib_device register and unregister, deliberately kept
 *                   outside state->lock so sleeping registration cannot
 *                   invert against verbs operations.
 * tqp->lock         (spin, irqsave) QP lists, attributes, state and
 *                   timeouts. A leaf: no other lock is taken while it is
 *                   held.
 * dmabuf_engine.ring_lock
 *                   Rail ring stop and progress. Taken only by trylock
 *                   under state->lock, but with a blocking mutex_lock()
 *                   under dmabuf_engine.rx_lock, through
 *                   tbv_dmabuf_stop_rail() on a credit mismatch.
 * tqp->rx_lock      RX reassembly, reorder table, ack history, RNR state.
 *                   Dropped on purpose while a BLOCK_WRITE fragment is
 *                   placed.
 * tqp->ack_lock     SEND_ACK ordering. tqp->post_lock serialises post_send.
 *                   Both are only taken when DMA-BUF verbs are enabled.
 * read->data_lock   RDMA READ response fragment data.
 * path->tx_lock     TX queues, credits and reservations.
 * send->lock, cq->lock, verbs_qps_xa, verbs_mrs_xa, mr->host_dma_lock
 *                   Leaves.
 *
 * No nesting was observed for path->build_lock, dmabuf_engine.tx_lock,
 * cache_lock, submit_lock, credit_lock or progress_lock. That is an absence
 * of evidence, not a guarantee that they are leaves.
 *
 * _locked functions whose callers hold one identifiable lock assert it with
 * lockdep_assert_held(); a kernel built with CONFIG_PROVE_LOCKING checks
 * those assertions and this order at run time.
 *
 * The module has exactly one struct tbv_state. That is what makes an
 * assertion on &dev->state->lock, &peer->state->lock or &tqp->owner->lock
 * name the lock the caller holds, even where the caller locked it through a
 * different pointer.
 */

#define TBV_IBDEV_MAX_CQE 4096

/* Private layouts shared by object allocation and the data path. CQ entries
 * and notifications are protected by lock. MR lookup/closing is protected
 * by owner->verbs_mrs_xa; each data-path use holds refs until completion. */
struct tbv_cq {
	struct ib_cq base;
	struct tbv_state *owner;
	spinlock_t lock;
	struct ib_wc *entries;
	u32 cqe;
	u32 head;
	u32 tail;
	u32 count;
	bool notify_armed;
	bool overflowed;
};

struct tbv_host_dma_map {
	struct device *dev;
	struct sg_table sgt;
	dma_addr_t *pages;
	unsigned long npages;
};

struct tbv_mr {
	struct ib_mr base;
	struct tbv_state *owner;
	struct ib_umem *umem;
	struct tbv_dmabuf_mr *dmabuf;
	/*
	 * The umem's pages in order, so a copy finds the page of an offset
	 * directly; scatterlist copy helpers walk from the first entry, which
	 * costs one step per page before the offset. NULL when unindexed.
	 */
	struct page **pages;
	unsigned long npages;
	struct mutex host_dma_lock;
	struct tbv_host_dma_map host_dma[4];
	refcount_t refs;
	struct work_struct free_work;
	u64 start;
	u64 length;
	u64 virt_addr;
	int access;
	bool closing;
	bool dma_mr;
};

bool tbv_host_dma_eligible(struct tbv_mr *mr, u64 offset, u64 length);
void tbv_host_dma_debugfs_show(struct seq_file *s);
struct tbv_host_dma_map *tbv_host_dma_map(struct tbv_mr *mr, struct device *dev);
int tbv_host_dma_address(struct tbv_mr *mr, struct tbv_host_dma_map *map,
			 u64 offset, u32 bytes, dma_addr_t *address);

struct tbv_state *tbv_ibdev_state(struct ib_device *ibdev);
enum tbv_backend_type tbv_ibdev_backend(struct ib_device *ibdev);
void tbv_ibdev_cq_overflow(struct ib_qp *qp);

struct tbv_mr *tbv_mr_get(struct tbv_state *state, u32 key);

void tbv_mr_put(struct tbv_mr *mr);

int tbv_mr_copy_pages(struct tbv_mr *mr, size_t offset, void *buf,
			     size_t len, bool to_mr);

int tbv_umem_copy_to(struct tbv_mr *mr, u64 addr, const void *src,
			    size_t len);

int tbv_umem_copy_to_iova(struct tbv_mr *mr, u64 iova,
				 const void *src, size_t len);

int tbv_umem_iova_to_addr(struct tbv_mr *mr, u64 iova, size_t len,
				 u64 *addr_out);

int tbv_umem_copy_from_iova(struct tbv_mr *mr, u64 iova,
				   void *dst, size_t len);

int tbv_umem_page_from_addr(struct tbv_mr *mr, u64 addr, u32 max_len,
				   struct page **page_out,
				   u32 *page_off_out, u32 *len_out);

struct ib_mr *tbv_get_dma_mr(struct ib_pd *pd, int access);

struct ib_mr *tbv_reg_user_mr(struct ib_pd *pd, u64 start, u64 length,
				     u64 virt_addr, int access,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)
				     struct ib_dmah *dmah,
#endif
				     struct ib_udata *udata);

struct ib_mr *tbv_reg_user_mr_dmabuf(struct ib_pd *pd, u64 offset,
					   u64 length, u64 virt_addr, int fd,
					   int access,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)
					   struct ib_dmah *dmah,
#endif
					   struct uverbs_attr_bundle *attrs);

int tbv_dereg_mr(struct ib_mr *ibmr, struct ib_udata *udata);

int tbv_create_cq(struct ib_cq *cq, const struct ib_cq_init_attr *attr,
			 struct uverbs_attr_bundle *attrs);

int tbv_destroy_cq(struct ib_cq *cq, struct ib_udata *udata);

int tbv_cq_push(struct tbv_cq *tcq, const struct ib_wc *wc);

int tbv_poll_cq(struct ib_cq *cq, int num_entries, struct ib_wc *wc);

int tbv_req_notify_cq(struct ib_cq *cq, enum ib_cq_notify_flags flags);

#endif
