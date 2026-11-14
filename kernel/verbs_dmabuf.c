// SPDX-License-Identifier: GPL-2.0
/* Verbs-owned DMA-BUF MRs and USB4 rendezvous. Payload never uses TCP.
 * The opt-in mode reserves the native rings for descriptors, in both
 * directions. Ordinary MRs use bounded coherent staging; DMA-BUF MRs do not.
 */
/*
 * Locking
 *
 *   dmabuf_engine.rx_lock > state->lock
 *   dmabuf_engine.rx_lock > dmabuf_engine.ring_lock
 *   state->lock           > dmabuf_engine.ring_lock (trylock only)
 *
 * ring_lock is only trylocked while state->lock is held; elsewhere,
 * including tbv_dmabuf_stop_rail() under rx_lock, it is a blocking lock.
 *
 * No nesting was observed for dmabuf_engine.tx_lock, cache_lock,
 * submit_lock, credit_lock or progress_lock. The full module order is in
 * verbs_internal.h.
 */
#define pr_fmt(fmt) "thunderbolt_ibverbs dmabuf: " fmt

#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/delay.h>
#include <linux/hrtimer.h>
#include <linux/io.h>
#include <linux/pci.h>
#include <linux/version.h>
#include <linux/module.h>
#include <linux/overflow.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/thunderbolt.h>
#include <linux/timekeeping.h>
#include <linux/utsname.h>
#include <rdma/ib_umem.h>
#include <rdma/ib_verbs.h>

#include "tbv.h"
#include "verbs_dmabuf.h"
#include "verbs_internal.h"
#include "verbs_control.h"
#include "perf.h"

static bool dmabuf_verbs;
module_param(dmabuf_verbs, bool, 0444);
MODULE_PARM_DESC(dmabuf_verbs,
	"Native descriptor verbs with DMA-BUF MRs and USB4 control; requires register_verbs=1");

static bool dmabuf_index = true;
module_param(dmabuf_index, bool, 0444);
MODULE_PARM_DESC(dmabuf_index, "Index DMA-BUF frame addresses once per NHI attachment");

/* Optional bounded progress for NHIs whose completion IRQ can be missed.
 * Use the ordinary serialized ring worker; never consume descriptors here. */
static uint dmabuf_poll_us;
module_param(dmabuf_poll_us, uint, 0444);
MODULE_PARM_DESC(dmabuf_poll_us,
	"Descriptor ring progress interval in microseconds; 0 keeps IRQ/10ms watchdog behavior");

static bool dmabuf_poll_control;
module_param(dmabuf_poll_control, bool, 0444);
MODULE_PARM_DESC(dmabuf_poll_control,
	"Also progress existing XDomain control rings while a verbs QP exists; requires dmabuf_poll_us");

/*
 * Opt-in for the AMD 158d/158e NHI ring ABI. kernel/compat/tb_ring_layout.c
 * probes the tb_ring and tb_nhi layout at build time and defines
 * TBV_TB_RING_LAYOUT only when it matches; any other layout, including a
 * lockdep kernel where tb_ring.lock_key is not empty, compiles the path out.
 * It was qualified on Linux 7.2.6 (Ubuntu, Framework Desktop), 7.2.11
 * (Fedora, Sixunited AXB35 / Bosgame M5) and 7.3.3 (Fedora, Bosgame M5);
 * other kernels with a matching layout warn at load.
 */
#if defined(CONFIG_X86_64) && defined(TBV_TB_RING_LAYOUT)
#define TBV_BULK_NHI_ABI 1
#endif
static bool dmabuf_batch_doorbell;
module_param(dmabuf_batch_doorbell, bool, 0444);
MODULE_PARM_DESC(dmabuf_batch_doorbell,
	"Publish a data-ring descriptor group with one MMIO doorbell on AMD 158d/158e NHIs; needs the probed tb_ring layout (x86_64, qualified on 7.2.6, 7.2.11, 7.3.3)");
static uint dmabuf_post_burst = 16;
module_param(dmabuf_post_burst, uint, 0444);
MODULE_PARM_DESC(dmabuf_post_burst, "Frames posted per rail turn, 1..512; default 16");
static atomic64_t dmabuf_bulk_calls, dmabuf_bulk_frames, dmabuf_bulk_fallback;
static atomic64_t dmabuf_progress_ticks, dmabuf_progress_skipped;
static bool dmabuf_coalesce_copy;
module_param(dmabuf_coalesce_copy, bool, 0444);
MODULE_PARM_DESC(dmabuf_coalesce_copy,
	"Copy each host-staged TX lane once instead of once per 4 KiB frame");
static atomic64_t dmabuf_copy_chunks, dmabuf_copy_bytes;
static bool dmabuf_reuse_buffers;
module_param(dmabuf_reuse_buffers, bool, 0444);
MODULE_PARM_DESC(dmabuf_reuse_buffers,
	"Cache one drained descriptor/staging batch per rail and direction (opt-in)");
static atomic64_t dmabuf_cache_hits, dmabuf_cache_misses, dmabuf_cache_bytes;
static atomic64_t dmabuf_cancel_unposted, dmabuf_cancel_drained, dmabuf_cancel_stopped;
static atomic64_t dmabuf_cancel_rx_drained, dmabuf_cancel_rx_stopped;
static atomic64_t dmabuf_host_tx_frames, dmabuf_host_rx_frames;

static bool dmabuf_profile;
module_param(dmabuf_profile, bool, 0444);
MODULE_PARM_DESC(dmabuf_profile, "Collect descriptor verbs phase timings in debugfs summary");

static bool dmabuf_rx_eager = true;
module_param(dmabuf_rx_eager, bool, 0444);
MODULE_PARM_DESC(dmabuf_rx_eager, "Reap RX on the final DMA callback instead of waiting for STATUS");

static bool dmabuf_parallel_post = true;
module_param(dmabuf_parallel_post, bool, 0444);
MODULE_PARM_DESC(dmabuf_parallel_post, "Post large descriptor batches concurrently on distinct NHIs");

static bool dmabuf_credits;
module_param(dmabuf_credits, bool, 0444);
MODULE_PARM_DESC(dmabuf_credits,
	"Pre-arm the next GPU SEND receive WQE so the peer can skip PREPARE; requires dmabuf_push_done=1 and dmabuf_control_ring=0");

static bool dmabuf_direct_post = true;
module_param(dmabuf_direct_post, bool, 0444);
MODULE_PARM_DESC(dmabuf_direct_post,
	"With credits, post an idle engine's credited SEND from ibv_post_send instead of the TX worker");

static uint dmabuf_credit_max = SZ_1M;
module_param(dmabuf_credit_max, uint, 0444);
MODULE_PARM_DESC(dmabuf_credit_max, "Largest SEND length a credit pre-arms, in bytes");

enum tbv_dmabuf_credit_counter {
	CREDIT_GRANTED, CREDIT_COMMITTED, CREDIT_ADOPTED, CREDIT_RELEASED,
	CREDIT_CANCELED, CREDIT_ABORTED, CREDIT_USED, CREDIT_RETURNED,
	CREDIT_STALE, CREDIT_MISMATCH, CREDIT_DIRECT, CREDIT_READOPTED,
	CREDIT_COUNTERS,
};

static const char * const dmabuf_credit_names[] = {
	"granted", "committed", "adopted", "released", "canceled", "aborted",
	"used", "returned", "stale", "mismatch", "direct", "readopted",
};

static atomic64_t dmabuf_credit_counters[CREDIT_COUNTERS];

static void tbv_dmabuf_credit_count(enum tbv_dmabuf_credit_counter counter)
{
	atomic64_inc(&dmabuf_credit_counters[counter]);
}

enum tbv_dmabuf_phase {
	TBV_DMABUF_QUEUE,
	TBV_DMABUF_JOB,
	TBV_DMABUF_TX_BUILD,
	TBV_DMABUF_PREPARE_TIME,
	TBV_DMABUF_TX_POST,
	TBV_DMABUF_TX_WAIT,
	TBV_DMABUF_STATUS_TIME,
	TBV_DMABUF_TX_RELEASE,
	TBV_DMABUF_TX_FINISH,
	TBV_DMABUF_RX_BUILD,
	TBV_DMABUF_RX_POST,
	TBV_DMABUF_RX_REAP,
	TBV_DMABUF_RX_RELEASE,
	TBV_DMABUF_PREPARE_WAIT,
	TBV_DMABUF_RNR_WAIT,
	TBV_DMABUF_STATUS_WAIT,
	TBV_DMABUF_DONE_WAIT,
	TBV_DMABUF_PHASE_COUNT,
};

static const char * const dmabuf_phase_names[] = {
	"queue", "job", "tx_build", "prepare", "tx_post", "tx_wait", "status",
	"tx_release", "tx_finish", "rx_build", "rx_post", "rx_reap", "rx_release",
	"prepare_wait", "rnr_wait", "status_wait", "done_wait",
};

static struct {
	atomic64_t calls;
	atomic64_t ns;
	atomic64_t max_ns;
} dmabuf_timings[TBV_DMABUF_PHASE_COUNT];

static u64 tbv_dmabuf_clock(void)
{
	return dmabuf_profile ? ktime_get_ns() : 0;
}

static void tbv_dmabuf_record(enum tbv_dmabuf_phase phase, u64 start)
{
	u64 ns, old;

	if (!start)
		return;
	ns = ktime_get_ns() - start;
	atomic64_inc(&dmabuf_timings[phase].calls);
	atomic64_add(ns, &dmabuf_timings[phase].ns);
	old = atomic64_read(&dmabuf_timings[phase].max_ns);
	while (old < ns) {
		u64 previous = atomic64_cmpxchg(&dmabuf_timings[phase].max_ns, old, ns);

		if (previous == old)
			break;
		old = previous;
	}
}

void tbv_dmabuf_debugfs_show(struct seq_file *s)
{
	u32 i;

	seq_printf(s, "dmabuf_verbs: %u\n", dmabuf_verbs);
	seq_printf(s, "dmabuf_index: %u\n", dmabuf_index);
	seq_printf(s, "dmabuf_profile: %u\n", dmabuf_profile);
	seq_printf(s, "dmabuf_reuse_buffers: %u\n", dmabuf_reuse_buffers);
	seq_printf(s, "dmabuf_cache_hits: %lld\n", atomic64_read(&dmabuf_cache_hits));
	seq_printf(s, "dmabuf_cancel_unposted: %lld\n", atomic64_read(&dmabuf_cancel_unposted));
	seq_printf(s, "dmabuf_cancel_drained: %lld\n", atomic64_read(&dmabuf_cancel_drained));
	seq_printf(s, "dmabuf_cancel_stopped: %lld\n", atomic64_read(&dmabuf_cancel_stopped));
	seq_printf(s, "dmabuf_cancel_rx_drained: %lld\n", atomic64_read(&dmabuf_cancel_rx_drained));
	seq_printf(s, "dmabuf_cancel_rx_stopped: %lld\n", atomic64_read(&dmabuf_cancel_rx_stopped));
	seq_printf(s, "dmabuf_cache_misses: %lld\n", atomic64_read(&dmabuf_cache_misses));
	seq_printf(s, "dmabuf_cache_bytes: %lld\n", atomic64_read(&dmabuf_cache_bytes));
	seq_printf(s, "dmabuf_host_tx_frames: %lld\n", atomic64_read(&dmabuf_host_tx_frames));
	seq_printf(s, "dmabuf_host_rx_frames: %lld\n", atomic64_read(&dmabuf_host_rx_frames));
	seq_printf(s, "dmabuf_coalesce_copy: %u\n", dmabuf_coalesce_copy);
	seq_printf(s, "dmabuf_copy_chunks: %lld\n", atomic64_read(&dmabuf_copy_chunks));
	seq_printf(s, "dmabuf_copy_bytes: %lld\n", atomic64_read(&dmabuf_copy_bytes));
	seq_printf(s, "dmabuf_batch_doorbell: %u\n", dmabuf_batch_doorbell);
	seq_printf(s, "dmabuf_post_burst: %u\n", dmabuf_post_burst);
	seq_printf(s, "dmabuf_bulk_calls: %lld\n", atomic64_read(&dmabuf_bulk_calls));
	seq_printf(s, "dmabuf_bulk_frames: %lld\n", atomic64_read(&dmabuf_bulk_frames));
	seq_printf(s, "dmabuf_bulk_fallback: %lld\n", atomic64_read(&dmabuf_bulk_fallback));
	seq_printf(s, "dmabuf_progress_ticks: %lld\n", atomic64_read(&dmabuf_progress_ticks));
	seq_printf(s, "dmabuf_progress_skipped: %lld\n", atomic64_read(&dmabuf_progress_skipped));
	seq_printf(s, "dmabuf_rx_eager: %u\n", dmabuf_rx_eager);
	seq_printf(s, "dmabuf_parallel_post: %u\n", dmabuf_parallel_post);
	seq_printf(s, "dmabuf_control_ring: %u\n", tbv_control_enabled());
	seq_printf(s, "dmabuf_push_done: %u\n", tbv_control_push_enabled());
	seq_printf(s, "dmabuf_credits: %u\n", tbv_dmabuf_credits_enabled());
	seq_printf(s, "dmabuf_credit_max: %u\n", dmabuf_credit_max);
	seq_printf(s, "dmabuf_direct_post: %u\n", dmabuf_direct_post);
	for (i = 0; i < CREDIT_COUNTERS; i++)
		seq_printf(s, "dmabuf_credit_%s: %lld\n", dmabuf_credit_names[i],
			   atomic64_read(&dmabuf_credit_counters[i]));
	if (!dmabuf_profile)
		return;
	for (i = 0; i < TBV_DMABUF_PHASE_COUNT; i++) {
		seq_printf(s, "dmabuf_%s_calls: %lld\n", dmabuf_phase_names[i],
			   atomic64_read(&dmabuf_timings[i].calls));
		seq_printf(s, "dmabuf_%s_ns: %lld\n", dmabuf_phase_names[i],
			   atomic64_read(&dmabuf_timings[i].ns));
		seq_printf(s, "dmabuf_%s_max_ns: %lld\n", dmabuf_phase_names[i],
			   atomic64_read(&dmabuf_timings[i].max_ns));
	}
}

struct tbv_dmabuf_page {
	dma_addr_t address;
	u32 bytes;
};

struct tbv_dmabuf_mapping {
	struct device *dev;
	struct dma_buf_attachment *attach;
	struct sg_table *sgt;
	struct tbv_dmabuf_page *pages;
};

struct tbv_dmabuf_mr {
	struct ib_umem_dmabuf *umem;
	struct mutex lock;
	u64 offset;
	u64 length;
	struct tbv_dmabuf_mapping maps[TBV_DMABUF_MAX_RAILS];
};

struct tbv_dmabuf_batch;

struct tbv_dmabuf_frame {
	struct ring_frame frame;
	struct tbv_dmabuf_batch *batch;
	struct tbv_dmabuf_lane *lane;
	u32 bytes;
	u8 eof;
};

struct tbv_dmabuf_cache {
	struct tbv_dmabuf_frame *frames;
	struct device *dev;
	void *staging;
	dma_addr_t staging_dma;
	bool leased, invalid;
};

struct tbv_dmabuf_lane {
	struct tbv_rail *rail;
	struct tbv_dmabuf_cache *cache;
	struct tbv_host_dma_map *host_map;
	struct tbv_dmabuf_frame *frames;
	struct device *dev;
	void *staging;
	dma_addr_t staging_dma;
	u32 first;
	u32 count;
	u32 staging_bytes;
	struct tbv_perf_id perf;
	u64 perf_post_started;
	atomic_t perf_pending;
	bool perf_active;
};

struct tbv_dmabuf_post_group {
	struct work_struct work;
	struct tbv_dmabuf_batch *batch;
	struct device *dev;
	int status;
	u64 perf_queued_ns;
};

struct tbv_dmabuf_batch {
	struct tbv_dmabuf_lane lanes[TBV_DMABUF_MAX_RAILS];
	struct tbv_dmabuf_post_group posts[TBV_DMABUF_MAX_RAILS];
	atomic_t pending;
	atomic_t error;
	wait_queue_head_t wait;
	u32 count;
	bool submitted;
	bool tx;
	/* tbv_dmabuf_cancel() was called for this batch's WR (tx_lock): no new
	 * PREPARE; armed frames are still posted and drained. */
	bool canceled;
	struct tbv_perf_id perf;
	u64 perf_dma_done_ns;
};

struct tbv_dmabuf_job {
	struct work_struct work;
	struct tbv_state *state;
	struct tbv_rail *route;
	struct tbv_native_data_header hdr;
	struct tbv_dmabuf_buffer buffer;
	u8 retries;
	u8 rnr_retries;
	unsigned long timeout;
	u64 queued_ns;
	u64 perf_queued_ns;
	struct tbv_perf_id perf;
	/* Frames point back at their batch, so it lives with the WR. */
	struct tbv_dmabuf_batch batch;
	/* Set when ibv_post_send() already sent COMMIT and posted the frames
	 * (tbv_dmabuf_tx_direct); the worker then only waits and finishes. */
	struct tbv_dmabuf_wire wire;
	struct tbv_ctrl_pending *pending;
	unsigned long deadline;
	int direct_ret;
	bool posted;
};

/* OPEN retains the WR between DMA batches; CACHED retains its final result.
 * SPECULATIVE has armed a credit's pages before its WR is known; last keeps
 * the previous WR's result for STATUS meanwhile. All transitions and the
 * buffer/control references are protected by rx_lock. */
enum tbv_dmabuf_rx_state {
	TBV_DMABUF_RX_IDLE,
	TBV_DMABUF_RX_OPEN,
	TBV_DMABUF_RX_ARMED,
	TBV_DMABUF_RX_CACHED,
	TBV_DMABUF_RX_SPECULATIVE,
};

struct tbv_dmabuf_rx {
	struct tbv_dmabuf_wire request;
	struct tbv_dmabuf_wire last;
	struct tbv_dmabuf_buffer buffer;
	struct tbv_dmabuf_batch batch;
	struct tbv_control *control;
	struct tbv_control *credit_ctrl;
	unsigned long deadline;
	unsigned long release_deadline;
	/* When an armed batch whose WR died stops waiting for its frames. */
	unsigned long cancel_deadline;
	u32 next_offset;
	int status;
	int last_status;
	/* A committed credit whose WQE was gone: fail it after its DMA. */
	int deferred;
	/* The credit a PREPARE adopted for this request. Every READY for it
	 * names the credit, also one answering a retried PREPARE whose first
	 * READY the sender never saw; otherwise the sender would RELEASE it. */
	u32 adopted_credit;
	enum tbv_dmabuf_rx_state state;
	bool last_valid;
	bool released;
	bool cancel_sent;
};

/* A data sender's view of one receiving host's credit. done is the highest
 * credit this host has committed, released, or seen adopted by a PREPARE;
 * older notifications are stale. */
struct tbv_dmabuf_credit {
	u8 host[16];
	struct tbv_dmabuf_wire wire;
	u32 done;
	bool used;
	bool valid;
	bool cancel;
};

#define TBV_DMABUF_CREDIT_HOSTS 4

/* One ordered TX stream and one RX transaction span all rails/links.
 * ring_lock serializes ring replacement; tx_lock protects cancellation. */
struct tbv_dmabuf_engine {
	struct workqueue_struct *tx_wq, *post_wq, *progress_wq;
	struct hrtimer progress_timer;
	struct work_struct progress_work;
	atomic_t progress_batches;
	spinlock_t progress_lock;
	bool progress_running;
	bool progress_stopping;
	struct mutex rx_lock, ring_lock, tx_lock;
	struct mutex cache_lock;
	/* Orders submissions with direct posting: queued or running TX jobs. */
	struct mutex submit_lock;
	atomic_t tx_jobs;
	struct tbv_state *state;
	void *active_context;
	struct tbv_dmabuf_batch *active_batch;
	struct tbv_dmabuf_rx rx;
	struct delayed_work rx_watchdog;
	struct work_struct rx_complete_work;
	atomic64_t token;
	/* Receiver: the stream to pre-arm once its RQ has a WQE (rx_lock). */
	struct work_struct credit_work;
	wait_queue_head_t rx_wait;
	void *want_owner;
	struct tbv_dmabuf_wire want;
	struct tbv_control *want_ctrl;
	u32 credit_seq;
	/* Sender: credits granted to this host, released on tx_wq. */
	struct work_struct settle_work;
	spinlock_t credit_lock;
	wait_queue_head_t credit_wait;
	struct tbv_dmabuf_credit credits[TBV_DMABUF_CREDIT_HOSTS];
	u32 credit_victim;
};

static struct tbv_dmabuf_engine dmabuf_engine = {
	.rx_lock = __MUTEX_INITIALIZER(dmabuf_engine.rx_lock),
	.ring_lock = __MUTEX_INITIALIZER(dmabuf_engine.ring_lock),
	.tx_lock = __MUTEX_INITIALIZER(dmabuf_engine.tx_lock),
	.cache_lock = __MUTEX_INITIALIZER(dmabuf_engine.cache_lock),
	.submit_lock = __MUTEX_INITIALIZER(dmabuf_engine.submit_lock),
	.tx_jobs = ATOMIC_INIT(0),
	.progress_batches = ATOMIC_INIT(0),
	.progress_lock = __SPIN_LOCK_UNLOCKED(dmabuf_engine.progress_lock),
	.token = ATOMIC64_INIT(0),
	.rx_wait = __WAIT_QUEUE_HEAD_INITIALIZER(dmabuf_engine.rx_wait),
	.credit_lock = __SPIN_LOCK_UNLOCKED(dmabuf_engine.credit_lock),
	.credit_wait = __WAIT_QUEUE_HEAD_INITIALIZER(dmabuf_engine.credit_wait),
};

static bool tbv_dmabuf_rx_active(const struct tbv_dmabuf_rx *rx)
{
	return rx->state == TBV_DMABUF_RX_OPEN || rx->state == TBV_DMABUF_RX_ARMED;
}

bool tbv_dmabuf_credits_enabled(void)
{
	return dmabuf_verbs && dmabuf_credits && tbv_control_push_enabled() &&
		!tbv_control_enabled();
}

bool tbv_dmabuf_verbs_enabled(void)
{
	return dmabuf_verbs;
}

struct tbv_dmabuf_mr *tbv_dmabuf_import(struct ib_device *device, u64 offset,
				       u64 length, int fd, int access)
{
	struct tbv_dmabuf_mr *mr;
	u64 end;

	if (!dmabuf_verbs)
		return ERR_PTR(-EOPNOTSUPP);
	if (!length || length > (256ull << 20) || length % PAGE_SIZE || offset % PAGE_SIZE ||
	    check_add_overflow(offset, length, &end) || end > ULONG_MAX)
		return ERR_PTR(-EINVAL);
	mr = kzalloc(sizeof(*mr), GFP_KERNEL);
	if (!mr)
		return ERR_PTR(-ENOMEM);
	/* The RDMA core handles fd/range validation and exporter pinning. This
	 * mapping is only the lifetime anchor; descriptor IOVAs always come
	 * from an attachment to the actual NHI device below. */
	mr->umem = ib_umem_dmabuf_get_pinned(device, offset, length, fd, access);
	if (IS_ERR(mr->umem)) {
		int ret = PTR_ERR(mr->umem);

		kfree(mr);
		return ERR_PTR(ret);
	}
	if (mr->umem->attach->dmabuf->size > (256ull << 20)) {
		ib_umem_release(&mr->umem->umem);
		kfree(mr);
		return ERR_PTR(-E2BIG);
	}
	mutex_init(&mr->lock);
	mr->offset = offset;
	mr->length = length;
	return mr;
}

void tbv_dmabuf_release(struct tbv_dmabuf_mr *mr)
{
	u32 i;

	if (!mr)
		return;
	for (i = 0; i < TBV_DMABUF_MAX_RAILS; i++) {
		struct tbv_dmabuf_mapping *map = &mr->maps[i];

		kvfree(map->pages);
		if (map->sgt)
			dma_buf_unmap_attachment_unlocked(map->attach, map->sgt,
							 DMA_BIDIRECTIONAL);
		if (map->attach)
			dma_buf_detach(map->attach->dmabuf, map->attach);
		put_device(map->dev);
	}
	ib_umem_release(&mr->umem->umem);
	mutex_destroy(&mr->lock);
	kfree(mr);
}

/* Entries retain each SG boundary: indexing never makes an NHI descriptor
 * span two entries. The padded receive span is checked by the same lookup. */
static int tbv_dmabuf_index_map(struct tbv_dmabuf_mr *mr,
				struct tbv_dmabuf_mapping *map)
{
	struct scatterlist *sg;
	u64 skip = mr->offset;
	u32 count = mr->length / TBV_DMABUF_FRAME_BYTES;
	u32 page = 0, i;

	map->pages = kvcalloc(count, sizeof(*map->pages), GFP_KERNEL);
	if (!map->pages)
		return -ENOMEM;
	for_each_sgtable_dma_sg(map->sgt, sg, i) {
		u32 len = sg_dma_len(sg);

		if (skip >= len) {
			skip -= len;
			continue;
		}
		while (page < count && skip < len) {
			struct tbv_dmabuf_page *entry = &map->pages[page++];
			u64 address;

			entry->bytes = min_t(u64, TBV_DMABUF_FRAME_BYTES, len - skip);
			if (check_add_overflow((u64)sg_dma_address(sg), skip, &address) ||
			    address > ~(dma_addr_t)0 ||
			    entry->bytes - 1 > ~(dma_addr_t)0 - address)
				return -EFAULT;
			entry->address = address;
			skip += TBV_DMABUF_FRAME_BYTES;
		}
		if (page == count)
			return 0;
		skip -= len;
	}
	return -EFAULT;
}

static int tbv_dmabuf_address(struct tbv_dmabuf_mr *mr, struct device *dev,
			      u64 offset, u32 bytes, dma_addr_t *address)
{
	struct tbv_dmabuf_mapping *map = NULL;
	struct scatterlist *sg;
	u64 skip;
	u64 perf_attach = 0;
	u32 i;
	int ret = -EFAULT;

	if (!bytes || offset % TBV_DMABUF_FRAME_BYTES ||
	    offset >= mr->length || bytes > mr->length - offset ||
	    check_add_overflow(offset, mr->offset, &skip))
		return -EFAULT;
	mutex_lock(&mr->lock);
	for (i = 0; i < TBV_DMABUF_MAX_RAILS; i++)
		if (mr->maps[i].dev == dev) {
			map = &mr->maps[i];
			break;
		}
	if (!map) {
		perf_attach = tbv_perf_clock();
		for (i = 0; i < TBV_DMABUF_MAX_RAILS; i++)
			if (!mr->maps[i].dev) {
				map = &mr->maps[i];
				break;
			}
		if (!map) {
			ret = -ENOSPC;
			goto out;
		}
		map->attach = dma_buf_attach(mr->umem->attach->dmabuf, dev);
		if (IS_ERR(map->attach)) {
			ret = PTR_ERR(map->attach);
			map->attach = NULL;
			goto out;
		}
		map->sgt = dma_buf_map_attachment_unlocked(map->attach,
							  DMA_BIDIRECTIONAL);
		if (IS_ERR(map->sgt)) {
			ret = PTR_ERR(map->sgt);
			map->sgt = NULL;
			dma_buf_detach(map->attach->dmabuf, map->attach);
			map->attach = NULL;
			goto out;
		}
		map->dev = get_device(dev);
		if (dmabuf_index) {
			ret = tbv_dmabuf_index_map(mr, map);
			if (ret) {
				kvfree(map->pages);
				map->pages = NULL;
				dma_buf_unmap_attachment_unlocked(map->attach, map->sgt,
							 DMA_BIDIRECTIONAL);
				dma_buf_detach(map->attach->dmabuf, map->attach);
				put_device(map->dev);
				memset(map, 0, sizeof(*map));
				goto out;
			}
		}
	}
	if (map->pages) {
		struct tbv_dmabuf_page *page = &map->pages[offset / TBV_DMABUF_FRAME_BYTES];

		if (bytes <= page->bytes) {
			*address = page->address;
			ret = 0;
		}
		goto out;
	}
	for_each_sgtable_dma_sg(map->sgt, sg, i) {
		u64 a;

		if (skip >= sg_dma_len(sg)) {
			skip -= sg_dma_len(sg);
			continue;
		}
		/* An NHI descriptor cannot cross an SG entry. */
		if (bytes > sg_dma_len(sg) - skip ||
		    check_add_overflow((u64)sg_dma_address(sg), skip, &a) ||
		    a > ~(dma_addr_t)0 || bytes - 1 > ~(dma_addr_t)0 - a)
			break;
		*address = a;
		ret = 0;
		break;
	}
out:
	mutex_unlock(&mr->lock);
	if (perf_attach) {
		struct tbv_perf_id id = { .bytes = mr->length };

		tbv_perf_span(&id, "mr_attach_index", perf_attach, ret, dmabuf_index);
	}
	return ret;
}

/* Raw frames have no operation identifier. A failed batch quarantines both
 * directions until the physical connection/module is recreated. Draining
 * precedes every release of frame metadata, mappings and verbs references. */
/* cache_lock protects ownership only. It is never held while draining rings. */
static void tbv_dmabuf_cache_free(struct tbv_dmabuf_cache *cache)
{
	if (cache->staging) {
		dma_free_coherent(cache->dev, SZ_2M, cache->staging, cache->staging_dma);
		atomic64_sub(SZ_2M, &dmabuf_cache_bytes);
	}
	kfree(cache->frames);
	put_device(cache->dev);
	kfree(cache);
}

static void tbv_dmabuf_cache_purge(struct tbv_rail *rail)
{
	unsigned int i;

	mutex_lock(&dmabuf_engine.cache_lock);
	for (i = 0; i < 2; i++) {
		struct tbv_dmabuf_cache *cache = rail->dmabuf_cache[i];

		if (!cache)
			continue;
		cache->invalid = true;
		if (!cache->leased) {
			rail->dmabuf_cache[i] = NULL;
			tbv_dmabuf_cache_free(cache);
		}
	}
	mutex_unlock(&dmabuf_engine.cache_lock);
}

static int tbv_dmabuf_cache_get(struct tbv_dmabuf_lane *lane, bool tx, bool host)
{
	struct tbv_dmabuf_cache **slot = &lane->rail->dmabuf_cache[tx];
	struct tbv_dmabuf_cache *cache;
	int ret = 0;

	mutex_lock(&dmabuf_engine.cache_lock);
	cache = *slot;
	if (cache && (cache->leased || cache->invalid)) {
		ret = -EBUSY;
		goto out;
	}
	if (!cache) {
		cache = kzalloc(sizeof(*cache), GFP_KERNEL);
		if (!cache) {
			ret = -ENOMEM;
			goto out;
		}
		cache->dev = get_device(lane->dev);
		cache->frames = kcalloc(TBV_DMABUF_BATCH_FRAMES, sizeof(*cache->frames), GFP_KERNEL);
		if (!cache->frames) {
			tbv_dmabuf_cache_free(cache);
			ret = -ENOMEM;
			goto out;
		}
		*slot = cache;
		atomic64_inc(&dmabuf_cache_misses);
	} else {
		atomic64_inc(&dmabuf_cache_hits);
	}
	if (host && !cache->staging) {
		cache->staging = dma_alloc_coherent(lane->dev, SZ_2M, &cache->staging_dma, GFP_KERNEL);
		if (!cache->staging) {
			ret = -ENOMEM;
			goto out;
		}
		atomic64_add(SZ_2M, &dmabuf_cache_bytes);
	}
	cache->leased = true;
	lane->cache = cache;
	lane->frames = cache->frames;
	memset(lane->frames, 0, TBV_DMABUF_BATCH_FRAMES * sizeof(*lane->frames));
	if (host) {
		lane->staging = cache->staging;
		lane->staging_dma = cache->staging_dma;
		lane->staging_bytes = lane->count * TBV_DMABUF_FRAME_BYTES;
	}
out:
	mutex_unlock(&dmabuf_engine.cache_lock);
	return ret;
}

static void tbv_dmabuf_cache_put(struct tbv_dmabuf_lane *lane, bool abort, bool tx)
{
	struct tbv_dmabuf_cache *cache = lane->cache;

	mutex_lock(&dmabuf_engine.cache_lock);
	cache->leased = false;
	if (abort || cache->invalid) {
		lane->rail->dmabuf_cache[tx] = NULL;
		tbv_dmabuf_cache_free(cache);
	}
	mutex_unlock(&dmabuf_engine.cache_lock);
}

static void tbv_dmabuf_stop_rail(struct tbv_rail *rail)
{
	mutex_lock(&dmabuf_engine.ring_lock);
	if (!READ_ONCE(rail->rings_stopped)) {
		WRITE_ONCE(rail->rings_stopped, true);
		if (!READ_ONCE(rail->dmabuf_quarantine_reason))
			WRITE_ONCE(rail->dmabuf_quarantine_reason, 1);
		tb_ring_stop(rail->path.tx_ring);
		tb_ring_stop(rail->path.rx_ring);
		pr_warn_ratelimited("rail %#x stopped after a data error or abort (reason %d); recreate the connection to use it again\n",
				    rail->rail_id, rail->dmabuf_quarantine_reason);
	}
	mutex_unlock(&dmabuf_engine.ring_lock);
	tbv_dmabuf_cache_purge(rail);
}

/* Reboot/poweroff: stop the rail's data rings before the NHI's own shutdown
 * removes the domain. Sets the rings_stopped marker, so tbv_path_destroy()
 * does not stop them a second time (tb_ring_stop() warns on a stopped ring).
 * Bounded: a batch holding ring_lock must not be able to stall shutdown. */
int tbv_dmabuf_quiesce_rail(struct tbv_rail *rail)
{
	int tries = 1000;

	if (!rail->path.tx_ring || !rail->path.rx_ring ||
	    (rail->path.state != TBV_PATH_RING_STARTED &&
	     rail->path.state != TBV_PATH_TUNNEL_ENABLED))
		return 0;
	while (!mutex_trylock(&dmabuf_engine.ring_lock)) {
		if (!--tries)
			return -EBUSY;
		msleep(1);
	}
	if (!READ_ONCE(rail->rings_stopped)) {
		WRITE_ONCE(rail->rings_stopped, true);
		tb_ring_stop(rail->path.tx_ring);
		tb_ring_stop(rail->path.rx_ring);
	}
	mutex_unlock(&dmabuf_engine.ring_lock);
	return 0;
}

static void tbv_dmabuf_batch_drop(struct tbv_dmabuf_batch *batch, bool abort)
{
	u64 start = tbv_dmabuf_clock();
	u64 perf_start = tbv_perf_clock();
	struct tbv_perf_id perf = batch->perf;
	bool tx = batch->tx;
	u32 i;

	for (i = 0; i < batch->count; i++) {
		struct tbv_dmabuf_lane *lane = &batch->lanes[i];

		if (batch->submitted && lane->rail) {
			if (abort) {
				tbv_dmabuf_stop_rail(lane->rail);
			} else {
				struct tb_ring *ring = batch->tx ? lane->rail->path.tx_ring :
								 lane->rail->path.rx_ring;

				/* A zero pending count precedes the callback's last
				 * wakeup. Wait for its return before freeing its frame
				 * or the batch waitqueue, including on success. */
				flush_work(&ring->work);
			}
		}
		if (lane->host_map)
			dma_sync_sgtable_for_cpu(lane->dev, &lane->host_map->sgt, DMA_BIDIRECTIONAL);
		if (lane->cache)
			tbv_dmabuf_cache_put(lane, abort, batch->tx);
		else if (lane->staging)
			dma_free_coherent(lane->dev, lane->staging_bytes,
					  lane->staging, lane->staging_dma);
		if (!lane->cache)
			kfree(lane->frames);
		if (lane->rail)
			tbv_rail_put(lane->rail);
	}
	if (dmabuf_poll_us && batch->submitted)
		atomic_dec(&dmabuf_engine.progress_batches);
	memset(batch, 0, sizeof(*batch));
	tbv_dmabuf_record(tx ? TBV_DMABUF_TX_RELEASE : TBV_DMABUF_RX_RELEASE, start);
	tbv_perf_span(&perf, "batch_release", perf_start, abort ? -ECANCELED : 0, 0);
}

static void tbv_dmabuf_batch_kick(struct tbv_dmabuf_batch *batch, bool tx);

/* How long a canceled WR's posted frames may take to leave the rings. */
#define TBV_DMABUF_CANCEL_DRAIN_MS 200

/* Waits for a posted batch's DMA without stopping its rings. True only if all
 * of its frames completed without error. */
static bool tbv_dmabuf_batch_drain(struct tbv_dmabuf_batch *batch,
				   unsigned long deadline)
{
	while (atomic_read(&batch->pending) && !atomic_read(&batch->error) &&
	       time_before(jiffies, deadline))
		if (!wait_event_timeout(batch->wait, !atomic_read(&batch->pending) ||
				atomic_read(&batch->error), msecs_to_jiffies(10)))
			tbv_dmabuf_batch_kick(batch, batch->tx);
	return !atomic_read(&batch->pending) && !atomic_read(&batch->error);
}

/*
 * QP destroy/flush. Stopping the rings mid-batch quarantines both rails until
 * the module is reloaded, so a canceled batch is finished instead: one the
 * receiver has not armed yet is never posted; once armed (READY or a credit's
 * COMMIT) the worker posts it anyway and lets it drain, bounded by
 * TBV_DMABUF_CANCEL_DRAIN_MS. QP destroy waits for the worker, which keeps the
 * buffer valid. A batch already posted is waited for here; only one that does
 * not drain is stopped.
 */
void tbv_dmabuf_cancel(void *context)
{
	struct tbv_dmabuf_batch *batch;
	u32 i;

	if (!dmabuf_verbs)
		return;
	mutex_lock(&dmabuf_engine.tx_lock);
	batch = dmabuf_engine.active_batch;
	if (dmabuf_engine.active_context != context || !batch)
		goto out;
	WRITE_ONCE(batch->canceled, true);
	if (!batch->submitted) {
		atomic64_inc(&dmabuf_cancel_unposted);
		goto out;
	}
	if (tbv_dmabuf_batch_drain(batch, jiffies +
				   msecs_to_jiffies(TBV_DMABUF_CANCEL_DRAIN_MS))) {
		atomic64_inc(&dmabuf_cancel_drained);
		goto out;
	}
	atomic64_inc(&dmabuf_cancel_stopped);
	for (i = 0; i < batch->count; i++) {
		WRITE_ONCE(batch->lanes[i].rail->dmabuf_quarantine_reason, 2);
		tbv_dmabuf_stop_rail(batch->lanes[i].rail);
	}
out:
	mutex_unlock(&dmabuf_engine.tx_lock);
}

bool tbv_dmabuf_rail_ready(struct tbv_rail *rail)
{
	return !READ_ONCE(rail->removing) && !READ_ONCE(rail->rings_stopped) &&
		rail->peer->remote_host_known && tbv_rail_data_ready(rail) &&
		(!tbv_control_required(rail->peer) || tbv_control_ready(rail->peer));
}

static void tbv_dmabuf_complete(struct tb_ring *ring, struct ring_frame *frame,
				bool canceled)
{
	struct tbv_dmabuf_frame *f = container_of(frame, struct tbv_dmabuf_frame,
						  frame);
	struct tbv_path *path = &f->lane->rail->path;
	struct tbv_state *state = path->rail->peer->state;
	int error = 0;

	/* A one-packet short frame carries only its EOF PDF and reports SOF=0.
	 * Its exact byte count, EOF and CRC are still checked below. */
	if (canceled)
		error = -ECANCELED;
	else if (!ring->is_tx &&
		 ((frame->size ?: TBV_DMABUF_FRAME_BYTES) != f->bytes ||
		  (frame->sof != TBV_DATA_PDF_FRAME_START &&
		   (f->bytes > 256 || frame->sof)) || frame->eof != f->eof ||
		  frame->flags & (RING_DESC_CRC_ERROR | RING_DESC_BUFFER_OVERRUN))) {
		error = -EBADMSG;
		pr_debug("RX mismatch size=%u expected=%u sof=%u eof=%u expected_eof=%u flags=%x\n",
			 frame->size, f->bytes, frame->sof, frame->eof, f->eof, frame->flags);
	}
	if (error)
		atomic_cmpxchg(&f->batch->error, 0, error);
#ifndef TBV_DISABLE_PROFILING
	if (f->lane->perf_active) {
		int left = atomic_dec_return(&f->lane->perf_pending);

		if (left == (int)f->lane->count - 1)
			tbv_perf_mark(&f->lane->perf, "dma_first", error, left);
		if (!left)
			tbv_perf_mark(&f->lane->perf, "dma_last", error, 0);
	}
#endif
	if (canceled) {
		atomic64_inc(ring->is_tx ? &state->data_tx_canceled :
					 &state->data_rx_canceled);
	} else if (ring->is_tx) {
		atomic64_inc(&state->data_tx_completed);
		atomic64_inc(&path->data_tx_completed);
		atomic64_add(f->bytes, &path->tx_bytes);
	} else {
		atomic64_inc(&state->data_rx_completed);
		atomic64_inc(&path->data_rx_completed);
		atomic64_add(f->bytes, &path->rx_bytes);
		if (error)
			atomic64_inc(&state->data_rx_bad_frame);
	}
	if (atomic_dec_and_test(&f->batch->pending) || error) {
#ifndef TBV_DISABLE_PROFILING
		if (!atomic_read(&f->batch->pending)) {
			WRITE_ONCE(f->batch->perf_dma_done_ns, tbv_perf_clock());
			tbv_perf_mark(&f->batch->perf, "batch_dma_done", error, 0);
		}
#endif
		wake_up_all(&f->batch->wait);
		/* Reaping may drain ring callbacks and take the RX mutex. Keep it
		 * off this callback's ring worker; STATUS still confirms placement. */
		if (!ring->is_tx && (dmabuf_rx_eager || tbv_control_enabled() ||
				     tbv_control_push_enabled()))
			queue_work(system_highpri_wq, &dmabuf_engine.rx_complete_work);
	}
}

/* A credit RELEASE may nest inside an ordinary TX batch which already leases
 * the rail cache. Its bounded filler must own private metadata/staging; it must
 * never steal, clear, or wait for the outer batch's lease. The ordered TX queue
 * permits at most one such filler (at most dmabuf_credit_max bytes) at a time. */
static int tbv_dmabuf_lane_alloc(struct tbv_dmabuf_lane *lane,
			       const struct tbv_dmabuf_buffer *buffer, bool tx)
{
	if (dmabuf_reuse_buffers && !buffer->uncached) {
		int ret = tbv_dmabuf_cache_get(lane, tx, !buffer->mr && !buffer->host_mr);

		if (ret)
			return ret;
	} else {
		lane->frames = kcalloc(lane->count, sizeof(*lane->frames), GFP_KERNEL);
		if (!lane->frames)
			return -ENOMEM;
	}
	if (!buffer->mr && !buffer->host_mr && !lane->cache) {
		lane->staging_bytes = lane->count * TBV_DMABUF_FRAME_BYTES;
		lane->staging = dma_alloc_coherent(lane->dev, lane->staging_bytes,
						   &lane->staging_dma, GFP_KERNEL);
		if (!lane->staging)
			return -ENOMEM;
	}
	return 0;
}

static int tbv_dmabuf_batch_alloc_impl(struct tbv_dmabuf_batch *batch,
				  const struct tbv_dmabuf_wire *m,
				  const struct tbv_dmabuf_buffer *buffer, bool tx)
{
	u32 frames = DIV_ROUND_UP(m->bytes, TBV_DMABUF_FRAME_BYTES);
	u32 i;

	atomic_set(&batch->pending, frames);
	atomic_set(&batch->error, 0);
	init_waitqueue_head(&batch->wait);
	batch->tx = tx;
	for (i = 0; i < batch->count; i++) {
		struct tbv_dmabuf_lane *lane = &batch->lanes[i];
		struct tb_ring *ring = tx ? lane->rail->path.tx_ring :
					   lane->rail->path.rx_ring;
		u32 end = DIV_ROUND_UP((u64)frames * (i + 1), batch->count);
		u32 n;
		u64 perf_start;

		lane->first = DIV_ROUND_UP((u64)frames * i, batch->count);
		lane->count = end - lane->first;
		lane->dev = tb_ring_dma_device(ring);
		lane->perf = batch->perf;
		/* rail_id is local to a link; the wire lane is unique across links. */
		lane->perf.rail = i;
		lane->perf_active = !!tbv_perf_clock();
		if (lane->perf_active)
			atomic_set(&lane->perf_pending, lane->count);
		if (lane->count >= ring->size || !tbv_dma_device_ready(lane->dev))
			return -ENODEV;
		if (!lane->count)
			continue;
		if (buffer->host_mr) {
			lane->host_map = tbv_host_dma_map(buffer->host_mr, lane->dev);
			if (IS_ERR(lane->host_map)) {
				int ret = PTR_ERR(lane->host_map);

				lane->host_map = NULL;
				return ret;
			}
			dma_sync_sgtable_for_device(lane->dev, &lane->host_map->sgt, DMA_BIDIRECTIONAL);
		}
		perf_start = tbv_perf_clock();
		{
			int ret = tbv_dmabuf_lane_alloc(lane, buffer, tx);

			if (ret)
				return ret;
		}
		tbv_perf_span(&lane->perf, "lane_alloc", perf_start, 0, lane->count);
		perf_start = tbv_perf_clock();
		for (n = 0; n < lane->count; n++) {
			struct tbv_dmabuf_frame *f = &lane->frames[n];
			u32 offset = (lane->first + n) * TBV_DMABUF_FRAME_BYTES;
			int ret;

			INIT_LIST_HEAD(&f->frame.list);
			f->batch = batch;
			f->lane = lane;
			f->bytes = min_t(u32, TBV_DMABUF_FRAME_BYTES, m->bytes - offset);
			/* The per-rail last marker detects a lost interior frame;
			 * counting completions alone could accept shifted payload. */
			f->eof = n + 1 == lane->count ? TBV_DATA_PDF_FRAME_END : 2;
			f->frame.size = f->bytes % TBV_DMABUF_FRAME_BYTES;
			f->frame.sof = TBV_DATA_PDF_FRAME_START;
			f->frame.eof = f->eof;
			f->frame.callback = tbv_dmabuf_complete;
			if (buffer->mr) {
				ret = tbv_dmabuf_address(buffer->mr, lane->dev,
					buffer->offset + m->offset + offset,
					tx ? f->bytes : TBV_DMABUF_FRAME_BYTES,
					&f->frame.buffer_phy);
				if (ret)
					return ret;
			} else if (buffer->host_mr) {
				ret = tbv_host_dma_address(buffer->host_mr, lane->host_map,
					buffer->offset + m->offset + offset,
					TBV_DMABUF_FRAME_BYTES, &f->frame.buffer_phy);
				if (ret)
					return ret;
			} else {
				f->frame.buffer_phy = lane->staging_dma +
					n * TBV_DMABUF_FRAME_BYTES;
				if (tx && !dmabuf_coalesce_copy) {
					ret = buffer->copy(buffer->context, m->offset + offset,
						lane->staging + n * TBV_DMABUF_FRAME_BYTES,
						f->bytes, false);
					if (ret)
						return ret;
				}
			}
		}
	/* The lane covers a contiguous, validated slice of the same WR.
	 * Copy only its payload, retaining the existing per-MR bounds checks. */
	if (tx && !buffer->mr && !buffer->host_mr && dmabuf_coalesce_copy) {
		u32 offset = lane->first * TBV_DMABUF_FRAME_BYTES;
		u32 bytes = min_t(u32, lane->staging_bytes, m->bytes - offset);
		int ret = buffer->copy(buffer->context, m->offset + offset,
			lane->staging, bytes, false);

		if (ret)
			return ret;
		atomic64_inc(&dmabuf_copy_chunks);
		atomic64_add(bytes, &dmabuf_copy_bytes);
	}
		tbv_perf_span(&lane->perf, "lane_build", perf_start, 0, lane->count);
		if (buffer->host_mr)
			atomic64_add(lane->count, tx ? &dmabuf_host_tx_frames : &dmabuf_host_rx_frames);
	}
	return 0;
}

static int tbv_dmabuf_batch_alloc(struct tbv_dmabuf_batch *batch,
				  const struct tbv_dmabuf_wire *m,
				  const struct tbv_dmabuf_buffer *buffer, bool tx)
{
	u64 start = tbv_perf_clock();
	int ret;

	if (start) {
		batch->perf = tbv_perf_wire_id(m, tx);
		batch->perf.wr_id = buffer->wr_id;
	}
	ret = tbv_dmabuf_batch_alloc_impl(batch, m, buffer, tx);
	tbv_perf_span(&batch->perf, "batch_build", start, ret, buffer->mr != NULL);
	return ret;
}

/*
 * Optional data-ring publication for AMD 158d/158e NHIs (TBV_BULK_NHI_ABI).
 * The ordinary Thunderbolt worker still owns completion and cancellation.
 * No control-channel ring or GPU-direct polling ring uses this helper.
 */
struct tbv_bulk_desc {
	__le64 address;
	__le32 metadata;
	__le32 timestamp;
};

static int tbv_dmabuf_post_bulk(struct tbv_dmabuf_lane *lane, bool tx,
			       u32 first, u32 count)
{
#ifndef TBV_BULK_NHI_ABI
	if (!dmabuf_batch_doorbell || count < 2)
		return -EAGAIN;
	return -EOPNOTSUPP;
#else
	struct tb_ring *ring = tx ? lane->rail->path.tx_ring : lane->rail->path.rx_ring;
	struct tbv_bulk_desc *descs;
	struct pci_dev *pdev;
	unsigned long irqflags;
	u32 n, head, available;
	void __iomem *doorbell;

	BUILD_BUG_ON(sizeof(struct tbv_bulk_desc) != 16);
	BUILD_BUG_ON(sizeof(struct tb_ring) != 184);
	BUILD_BUG_ON(offsetof(struct tb_ring, descriptors) != 32);
	BUILD_BUG_ON(offsetof(struct tb_ring, in_flight) != 64);

	if (!dmabuf_batch_doorbell || count < 2)
		return -EAGAIN;
	if (first > lane->count || count > lane->count - first)
		return -EINVAL;
	if (!dev_is_pci(ring->nhi->dev))
		goto fallback;
	pdev = to_pci_dev(ring->nhi->dev);
	if (pdev->vendor != PCI_VENDOR_ID_AMD ||
	    (pdev->device != 0x158d && pdev->device != 0x158e) ||
	    !(ring->flags & RING_FLAG_FRAME) || ring->start_poll ||
	    ring->is_tx != tx || ring->hop <= 0 ||
	    ring->hop >= ring->nhi->hop_count)
		goto fallback;

	/* Same ring serialization as __tb_ring_enqueue() and ring_work(). */
	spin_lock_irqsave(&ring->lock, irqflags);
	if (!ring->running || READ_ONCE(ring->nhi->going_away)) {
		spin_unlock_irqrestore(&ring->lock, irqflags);
		return -ESHUTDOWN;
	}
	if (!list_empty(&ring->queue) || ring->size <= 1 ||
	    ring->size > U16_MAX || ring->head < 0 || ring->head >= ring->size ||
	    ring->tail < 0 || ring->tail >= ring->size) {
		spin_unlock_irqrestore(&ring->lock, irqflags);
		goto fallback;
	}
	available = (ring->tail + ring->size - ring->head - 1) % ring->size;
	if (available < count) {
		spin_unlock_irqrestore(&ring->lock, irqflags);
		goto fallback;
	}
	for (n = first; n < first + count; n++) {
		if (!list_empty(&lane->frames[n].frame.list)) {
			spin_unlock_irqrestore(&ring->lock, irqflags);
			return -EINVAL;
		}
	}
	descs = (struct tbv_bulk_desc *)ring->descriptors;
	head = ring->head;
	for (n = first; n < first + count; n++) {
		struct ring_frame *frame = &lane->frames[n].frame;
		struct tbv_bulk_desc *d = &descs[head];
		u32 flags = RING_DESC_POSTED;
		u32 metadata;

		/* The final descriptor notifies for the entire ordered group. */
		if (n + 1 == first + count)
			flags |= RING_DESC_INTERRUPT;
		metadata = tx ? frame->size | frame->eof << 12 | frame->sof << 16 :
			le32_to_cpu(READ_ONCE(d->metadata)) & GENMASK(19, 0);
		WRITE_ONCE(d->address, cpu_to_le64(frame->buffer_phy));
		WRITE_ONCE(d->timestamp, 0);
		WRITE_ONCE(d->metadata, cpu_to_le32(metadata | flags << 20));
		list_add_tail(&frame->list, &ring->in_flight);
		head = (head + 1) % ring->size;
	}
	ring->head = head;
	/* Descriptors must reach coherent memory before advancing hardware. */
	dma_wmb();
	doorbell = ring->nhi->iobase + (tx ? 0 : 0x08000) + ring->hop * 16 + 8;
	iowrite32(tx ? head << 16 : head, doorbell);
	spin_unlock_irqrestore(&ring->lock, irqflags);
	atomic64_inc(&dmabuf_bulk_calls);
	atomic64_add(count, &dmabuf_bulk_frames);
	if (tx) {
		atomic64_add(count, &lane->rail->peer->state->data_tx_posted);
		atomic64_add(count, &lane->rail->path.data_tx_posted);
	}
	return 0;
fallback:
	atomic64_inc(&dmabuf_bulk_fallback);
	return -EAGAIN;
#endif
}

static int tbv_dmabuf_lane_post(struct tbv_dmabuf_lane *lane, bool tx,
			       u32 first, u32 count)
{
	struct tb_ring *ring = tx ? lane->rail->path.tx_ring : lane->rail->path.rx_ring;
	u64 start = tbv_perf_clock();
	u32 n;
	int ret = 0;

	if (!first)
		lane->perf_post_started = start;
	if (!tbv_dmabuf_rail_ready(lane->rail)) {
		ret = -ENOTCONN;
		goto out;
	}
	ret = tbv_dmabuf_post_bulk(lane, tx, first, count);
	if (ret != -EAGAIN)
		goto out;
	ret = 0;
	for (n = first; n < first + count; n++) {
		ret = tx ? tb_ring_tx(ring, &lane->frames[n].frame) :
			   tb_ring_rx(ring, &lane->frames[n].frame);
		if (ret)
			goto out;
		if (tx) {
			atomic64_inc(&lane->rail->peer->state->data_tx_posted);
			atomic64_inc(&lane->rail->path.data_tx_posted);
		}
	}
out:
	tbv_perf_span(&lane->perf, "lane_post_chunk", start, ret, count);
	if (ret || first + count == lane->count)
		tbv_perf_span(&lane->perf, "lane_post", lane->perf_post_started,
			      ret, first + count);
	return ret;
}

static void tbv_dmabuf_post_group(struct work_struct *work)
{
	struct tbv_dmabuf_post_group *group = container_of(work,
						struct tbv_dmabuf_post_group, work);
	u64 started = tbv_perf_clock();
	struct tbv_perf_id perf = group->batch->perf;
	u32 i, first, frames = 0;

	/* A group is one NHI; its first nonempty rail labels the worker. */
	for (i = 0; i < group->batch->count; i++)
		if (group->batch->lanes[i].dev == group->dev &&
		    group->batch->lanes[i].count) {
			perf.rail = group->batch->lanes[i].perf.rail;
			break;
		}
	tbv_perf_interval(&perf, "post_queue", group->perf_queued_ns, started, 0, 0);

	for (i = 0; i < group->batch->count; i++)
		if (group->batch->lanes[i].dev == group->dev)
			frames = max(frames, group->batch->lanes[i].count);
	/* Start every rail before filling any one ring. Posting a whole rail
	 * first leaves its sibling idle during hundreds of MMIO writes. Small
	 * turns overlap their DMA while preserving each ring's frame order. */
	for (first = 0; first < frames; first += dmabuf_post_burst) {
		for (i = 0; i < group->batch->count; i++) {
			struct tbv_dmabuf_lane *lane = &group->batch->lanes[i];

			if (lane->dev != group->dev || first >= lane->count)
				continue;
			group->status = tbv_dmabuf_lane_post(lane, group->batch->tx,
				first, min_t(u32, dmabuf_post_burst, lane->count - first));
			if (group->status)
				goto out;
		}
	}
out:
	tbv_perf_span(&perf, "post_worker", started, group->status, 0);
}

static bool tbv_dmabuf_progress_active(void)
{
	return atomic_read(&dmabuf_engine.progress_batches) ||
		(dmabuf_poll_control &&
		 atomic_read(&dmabuf_engine.state->verbs_qps));
}

/* A running callback alone owns forwarding/restarting. Repeated submissions
 * must neither re-enqueue that callback nor postpone its progress deadline. */
static void tbv_dmabuf_progress_start(void)
{
	unsigned long flags;

	if (!dmabuf_verbs || !dmabuf_poll_us)
		return;
	spin_lock_irqsave(&dmabuf_engine.progress_lock, flags);
	if (!dmabuf_engine.progress_stopping && !dmabuf_engine.progress_running) {
		dmabuf_engine.progress_running = true;
		hrtimer_start(&dmabuf_engine.progress_timer,
			ns_to_ktime((u64)dmabuf_poll_us * NSEC_PER_USEC),
			HRTIMER_MODE_REL);
	}
	spin_unlock_irqrestore(&dmabuf_engine.progress_lock, flags);
}

void tbv_dmabuf_progress_arm(void)
{
	if (dmabuf_poll_control)
		tbv_dmabuf_progress_start();
}

/* Never wait for a callback while holding the lock that callback acquires. */
static void tbv_dmabuf_progress_stop(void)
{
	unsigned long flags;

	spin_lock_irqsave(&dmabuf_engine.progress_lock, flags);
	dmabuf_engine.progress_stopping = true;
	spin_unlock_irqrestore(&dmabuf_engine.progress_lock, flags);
	hrtimer_cancel(&dmabuf_engine.progress_timer);
	cancel_work_sync(&dmabuf_engine.progress_work);
	spin_lock_irqsave(&dmabuf_engine.progress_lock, flags);
	dmabuf_engine.progress_running = false;
	spin_unlock_irqrestore(&dmabuf_engine.progress_lock, flags);
}

/* The fallback timer only needs to wake an ordinary ring worker when its
 * next descriptor is complete. Do not reap, alter IRQs, or wait for a whole
 * group: partial completions must return credits and preserve ring progress.
 * The NHI ABI is the same pinned, opt-in ABI as bulk publication above. */
static bool tbv_dmabuf_ring_ready(struct tb_ring *ring)
{
#ifndef TBV_BULK_NHI_ABI
	return true;
#else
	struct tbv_bulk_desc *descs;
	struct pci_dev *pdev;
	unsigned long flags;
	bool ready = true;

	if (!dmabuf_batch_doorbell || !dev_is_pci(ring->nhi->dev))
		return true;
	pdev = to_pci_dev(ring->nhi->dev);
	if (pdev->vendor != PCI_VENDOR_ID_AMD ||
	    (pdev->device != 0x158d && pdev->device != 0x158e) ||
	    ring->start_poll)
		return true;
	spin_lock_irqsave(&ring->lock, flags);
	if (!ring->running || READ_ONCE(ring->nhi->going_away)) {
		ready = false;
	} else if (ring->descriptors && ring->size > 1 &&
		   ring->head >= 0 && ring->head < ring->size &&
		   ring->tail >= 0 && ring->tail < ring->size) {
		if (ring->head == ring->tail) {
			ready = !list_empty(&ring->queue);
		} else {
			descs = (struct tbv_bulk_desc *)ring->descriptors;
			ready = (le32_to_cpu(READ_ONCE(descs[ring->tail].metadata)) >> 20) &
				RING_DESC_COMPLETED;
		}
	}
	spin_unlock_irqrestore(&ring->lock, flags);
	return ready;
#endif
}

/* Hop zero belongs to the kernel XDomain control channel. Its ring pointers
 * share nhi->lock with allocation/free. Only its ordinary worker reaps it. */
static void tbv_dmabuf_progress_control(struct tb_nhi *nhi)
{
	unsigned long flags;

	spin_lock_irqsave(&nhi->lock, flags);
	if (!nhi->going_away) {
		if (nhi->tx_rings[0] && tbv_dmabuf_ring_ready(nhi->tx_rings[0]))
			schedule_work(&nhi->tx_rings[0]->work);
		if (nhi->rx_rings[0] && tbv_dmabuf_ring_ready(nhi->rx_rings[0]))
			schedule_work(&nhi->rx_rings[0]->work);
	}
	spin_unlock_irqrestore(&nhi->lock, flags);
}

static void tbv_dmabuf_progress_work(struct work_struct *work)
{
	struct tbv_state *state = dmabuf_engine.state;
	struct tbv_peer *peer;

	if (READ_ONCE(dmabuf_engine.progress_stopping) ||
	    !tbv_dmabuf_progress_active())
		return;
	/* A fallback tick must never queue behind the WR path that holds these
	 * locks: whoever holds them is making progress, the next tick follows. */
	if (!mutex_trylock(&state->lock)) {
		atomic64_inc(&dmabuf_progress_skipped);
		return;
	}
	list_for_each_entry(peer, &state->peers, node) {
		struct tbv_rail *rail;

		list_for_each_entry(rail, &peer->rails, node) {
			if (!mutex_trylock(&dmabuf_engine.ring_lock)) {
				atomic64_inc(&dmabuf_progress_skipped);
				continue;
			}
			if (!READ_ONCE(rail->removing) &&
			    !READ_ONCE(rail->rings_stopped)) {
				if (rail->path.tx_ring) {
					if (tbv_dmabuf_ring_ready(rail->path.tx_ring))
						schedule_work(&rail->path.tx_ring->work);
					if (dmabuf_poll_control)
						tbv_dmabuf_progress_control(rail->path.tx_ring->nhi);
				}
				if (rail->path.rx_ring && tbv_dmabuf_ring_ready(rail->path.rx_ring))
					schedule_work(&rail->path.rx_ring->work);
			}
			mutex_unlock(&dmabuf_engine.ring_lock);
		}
	}
	mutex_unlock(&state->lock);
}

static enum hrtimer_restart tbv_dmabuf_progress_timer(struct hrtimer *timer)
{
	unsigned long flags;
	enum hrtimer_restart restart = HRTIMER_NORESTART;

	spin_lock_irqsave(&dmabuf_engine.progress_lock, flags);
	if (dmabuf_engine.progress_stopping || !tbv_dmabuf_progress_active()) {
		dmabuf_engine.progress_running = false;
	} else {
		atomic64_inc(&dmabuf_progress_ticks);
		queue_work(dmabuf_engine.progress_wq, &dmabuf_engine.progress_work);
		hrtimer_forward_now(timer, ns_to_ktime((u64)dmabuf_poll_us * NSEC_PER_USEC));
		restart = HRTIMER_RESTART;
	}
	spin_unlock_irqrestore(&dmabuf_engine.progress_lock, flags);
	return restart;
}

static int tbv_dmabuf_batch_post(struct tbv_dmabuf_batch *batch, bool tx)
{
	u32 i, groups = 0, frames = 0;
	int ret = 0;

	for (i = 0; i < batch->count; i++) {
		struct tbv_dmabuf_lane *lane = &batch->lanes[i];
		u32 j;

		frames += lane->count;
		if (!lane->count)
			continue;
		for (j = 0; j < groups; j++)
			if (batch->posts[j].dev == lane->dev)
				break;
		if (j == groups) {
			batch->posts[groups].dev = lane->dev;
			batch->posts[groups].batch = batch;
			groups++;
		}
	}
	if (!frames)
		return 0;
	if (!batch->submitted) {
		batch->submitted = true;
		if (dmabuf_poll_us) {
			atomic_inc(&dmabuf_engine.progress_batches);
			tbv_dmabuf_progress_start();
		}
	}
	if (!dmabuf_parallel_post || groups < 2 || frames < 128) {
		for (i = 0; i < batch->count; i++) {
			ret = tbv_dmabuf_lane_post(&batch->lanes[i], tx, 0,
						 batch->lanes[i].count);
			if (ret)
				return ret;
		}
		return 0;
	}
	/* Lanes sharing an NHI remain sequential: its MMIO/locks are shared.
	 * Different NHIs can prepare descriptors together. Every worker returns
	 * before any batch metadata can be drained or released, including errors. */
	for (i = 0; i < groups; i++) {
		INIT_WORK(&batch->posts[i].work, tbv_dmabuf_post_group);
		batch->posts[i].perf_queued_ns = tbv_perf_clock();
		queue_work(dmabuf_engine.post_wq, &batch->posts[i].work);
	}
	for (i = 0; i < groups; i++) {
		flush_work(&batch->posts[i].work);
		if (!ret)
			ret = batch->posts[i].status;
	}
	return ret;
}

static void tbv_dmabuf_batch_kick(struct tbv_dmabuf_batch *batch, bool tx)
{
	u32 i;

	tbv_perf_mark(&batch->perf, "dma_watchdog", 0, atomic_read(&batch->pending));

	for (i = 0; i < batch->count; i++) {
		struct tbv_rail *rail = batch->lanes[i].rail;
		struct tb_ring *ring = tx ? rail->path.tx_ring : rail->path.rx_ring;

		mutex_lock(&dmabuf_engine.ring_lock);
		if (!READ_ONCE(rail->rings_stopped))
			schedule_work(&ring->work);
		mutex_unlock(&dmabuf_engine.ring_lock);
	}
}

static int tbv_dmabuf_batch_wait(struct tbv_dmabuf_batch *batch,
				 const struct tbv_dmabuf_buffer *buffer,
				 unsigned long deadline)
{
	while (atomic_read(&batch->pending) && !atomic_read(&batch->error)) {
		if (!buffer->alive(buffer->context))
			return -ECANCELED;
		if (time_after_eq(jiffies, deadline))
			return -ETIMEDOUT;
		/*
		 * Completion callbacks wake us directly. Only inspect the rings
		 * after a missed notification while DMA is still outstanding.
		 */
		if (!wait_event_timeout(batch->wait, !atomic_read(&batch->pending) ||
				atomic_read(&batch->error), msecs_to_jiffies(10)) &&
		    atomic_read(&batch->pending) && !atomic_read(&batch->error))
			tbv_dmabuf_batch_kick(batch, true);
	}
	return atomic_read(&batch->error);
}

static bool tbv_dmabuf_same(const struct tbv_dmabuf_wire *a,
			    const struct tbv_dmabuf_wire *b, bool batch)
{
	return tbv_dmabuf_wire_same(a, b, batch);
}

static void tbv_dmabuf_rx_notify(int status)
{
	struct tbv_control *ctrl = dmabuf_engine.rx.control;
	u64 start = tbv_perf_clock();

	dmabuf_engine.rx.control = NULL;
	tbv_control_done(ctrl, &dmabuf_engine.rx.request, status);
	tbv_perf_wire(&dmabuf_engine.rx.request, false, "done_send", start, status,
		      ctrl != NULL);
	tbv_control_put(ctrl);
}

static void tbv_dmabuf_credit_next(struct tbv_control *ctrl);
static void tbv_dmabuf_spec_cancel(void);
static void tbv_dmabuf_spec_drop(bool abort);
static bool tbv_dmabuf_spec_poll(void);

static void tbv_dmabuf_rx_finish(int status)
{
	struct tbv_dmabuf_rx *rx = &dmabuf_engine.rx;
	struct tbv_control *ctrl = rx->control;
	bool direct = rx->buffer.mr || rx->buffer.host_mr;
	u64 start;

	tbv_dmabuf_batch_drop(&rx->batch, !!status);
	if (rx->state == TBV_DMABUF_RX_ARMED)
		rx->state = TBV_DMABUF_RX_OPEN;
	/* tbv_dmabuf_cancel_recv() waits for an armed batch to end. */
	wake_up_all(&dmabuf_engine.rx_wait);
	start = tbv_perf_clock();
	if (tbv_dmabuf_rx_active(rx))
		status = rx->buffer.finish(rx->buffer.context, status);
	tbv_perf_wire(&rx->request, false, "rx_finish", start, status, 0);
	memset(&rx->buffer, 0, sizeof(rx->buffer));
	rx->state = TBV_DMABUF_RX_CACHED;
	rx->status = status;
	rx->deferred = 0;
	rx->last = rx->request;
	rx->last_status = status;
	rx->last_valid = true;
	if (ctrl)
		tbv_control_get(ctrl);
	tbv_dmabuf_rx_notify(status ?: TBV_DMABUF_DONE);
	/* After DONE, so the credit never delays this WR's completion. */
	if (!status && direct)
		tbv_dmabuf_credit_next(ctrl);
	tbv_control_put(ctrl);
}

static void tbv_dmabuf_want_clear(void)
{
	dmabuf_engine.want_owner = NULL;
	tbv_control_put(dmabuf_engine.want_ctrl);
	dmabuf_engine.want_ctrl = NULL;
}

void tbv_dmabuf_cancel_recv(void *owner)
{
	struct tbv_dmabuf_rx *rx = &dmabuf_engine.rx;

	if (!dmabuf_verbs)
		return;
	mutex_lock(&dmabuf_engine.rx_lock);
	if (dmabuf_engine.want_owner == owner)
		tbv_dmabuf_want_clear();
	if (rx->state == TBV_DMABUF_RX_SPECULATIVE && rx->buffer.owner == owner) {
		/* Its pages are armed: the peer must fill them before the rings
		 * can be reused. Without that, quarantine the rails as on abort. */
		tbv_dmabuf_spec_cancel();
		mutex_unlock(&dmabuf_engine.rx_lock);
		wait_event_timeout(dmabuf_engine.rx_wait,
			READ_ONCE(rx->state) != TBV_DMABUF_RX_SPECULATIVE ||
			READ_ONCE(rx->buffer.owner) != owner, msecs_to_jiffies(600));
		mutex_lock(&dmabuf_engine.rx_lock);
		if (rx->state == TBV_DMABUF_RX_SPECULATIVE && rx->buffer.owner == owner)
			tbv_dmabuf_spec_drop(true);
	}
	if (rx->state == TBV_DMABUF_RX_ARMED && rx->buffer.owner == owner) {
		/* READY told the sender to post into these descriptors, and it
		 * does even for a canceled WR. Canceling them instead stops both
		 * rails until reload: let the batch land, bounded like a sender's
		 * drain. The QP and its MR outlive this wait. */
		mutex_unlock(&dmabuf_engine.rx_lock);
		wait_event_timeout(dmabuf_engine.rx_wait,
			READ_ONCE(rx->state) != TBV_DMABUF_RX_ARMED ||
			READ_ONCE(rx->buffer.owner) != owner,
			msecs_to_jiffies(TBV_DMABUF_CANCEL_DRAIN_MS));
		mutex_lock(&dmabuf_engine.rx_lock);
		atomic64_inc(rx->state == TBV_DMABUF_RX_ARMED && rx->buffer.owner == owner ?
			     &dmabuf_cancel_rx_stopped : &dmabuf_cancel_rx_drained);
	}
	if (tbv_dmabuf_rx_active(rx) && rx->buffer.owner == owner)
		tbv_dmabuf_rx_finish(-ECANCELED);
	mutex_unlock(&dmabuf_engine.rx_lock);
}

static int tbv_dmabuf_rx_reap_impl(void)
{
	struct tbv_dmabuf_rx *rx = &dmabuf_engine.rx;
	u32 i;
	int ret = atomic_read(&rx->batch.error);

	if (rx->state != TBV_DMABUF_RX_ARMED)
		return TBV_DMABUF_DONE;
	if (ret)
		goto fail;
	if (atomic_read(&rx->batch.pending))
		return TBV_DMABUF_BUSY;
	dma_rmb();
	ret = rx->deferred;
	if (ret)
		goto fail;
	if (!rx->buffer.mr && !rx->buffer.host_mr) {
		for (i = 0; i < rx->batch.count; i++) {
			struct tbv_dmabuf_lane *lane = &rx->batch.lanes[i];
			u32 offset = lane->first * TBV_DMABUF_FRAME_BYTES;
			u32 bytes = min_t(u32, lane->staging_bytes, rx->request.bytes - offset);

			if (!lane->count)
				continue;
			ret = rx->buffer.copy(rx->buffer.context,
				rx->request.offset + offset, lane->staging, bytes, true);
			if (ret)
				goto fail;
		}
	}
	rx->next_offset = rx->request.offset + rx->request.bytes;
	if (rx->next_offset == rx->request.hdr.length) {
		tbv_dmabuf_rx_finish(0);
		return rx->status ?: TBV_DMABUF_DONE;
	} else {
		tbv_dmabuf_batch_drop(&rx->batch, false);
		rx->state = TBV_DMABUF_RX_OPEN;
		wake_up_all(&dmabuf_engine.rx_wait);
		tbv_dmabuf_rx_notify(TBV_DMABUF_DONE);
	}
	return TBV_DMABUF_DONE;
fail:
	tbv_dmabuf_rx_finish(ret);
	return ret;
}

static int tbv_dmabuf_rx_reap(void)
{
	u64 start = tbv_dmabuf_clock();
	u64 perf_start = tbv_perf_clock();
	int ret = tbv_dmabuf_rx_reap_impl();

	tbv_dmabuf_record(TBV_DMABUF_RX_REAP, start);
	tbv_perf_wire(&dmabuf_engine.rx.request, false, "rx_reap", perf_start, ret, 0);
	return ret;
}

static void tbv_dmabuf_rx_completed(struct work_struct *work)
{
	u64 start = tbv_perf_clock();

	mutex_lock(&dmabuf_engine.rx_lock);
	tbv_perf_wire(&dmabuf_engine.rx.request, false, "rx_worker_lock", start, 0, 0);
	tbv_perf_interval(&dmabuf_engine.rx.batch.perf, "rx_callback_worker",
		READ_ONCE(dmabuf_engine.rx.batch.perf_dma_done_ns), start, 0, 0);
	if (tbv_dmabuf_rx_active(&dmabuf_engine.rx) && dmabuf_engine.rx.state == TBV_DMABUF_RX_ARMED)
		tbv_dmabuf_rx_reap();
	else if (dmabuf_engine.rx.state == TBV_DMABUF_RX_SPECULATIVE)
		tbv_dmabuf_spec_poll();
	mutex_unlock(&dmabuf_engine.rx_lock);
}

static struct tbv_rail *tbv_dmabuf_find_lane(struct tbv_state *state,
					    const struct tbv_dmabuf_wire *m, u32 index)
{
	struct tbv_peer *peer;
	struct tbv_rail *found = NULL;

	mutex_lock(&state->lock);
	list_for_each_entry(peer, &state->peers, node) {
		struct tbv_rail *rail;

		if (!peer->xd->remote_uuid || !peer->remote_host_known ||
		    memcmp(peer->remote_host_id, m->src_host, 16) ||
		    memcmp(peer->xd->remote_uuid, m->lanes[index].domain, 16))
			continue;
		list_for_each_entry(rail, &peer->rails, node) {
			if (!tbv_dmabuf_rail_ready(rail) ||
			    rail->remote_transmit_path != m->lanes[index].path)
				continue;
			refcount_inc(&rail->refcnt);
			found = rail;
			goto out;
		}
	}
out:
	mutex_unlock(&state->lock);
	return found;
}

/* Whether a WR may use a credit: the same receiving QP, the PSN armed for
 * it, the exact armed length and the same wire lanes. Neither side can
 * redirect frames once descriptors are posted, so nothing else can match. */
static bool tbv_dmabuf_credit_match(const struct tbv_dmabuf_wire *credit,
				    const struct tbv_dmabuf_wire *m)
{
	return (m->hdr.opcode == TBV_NATIVE_DATA_OP_SEND ||
		m->hdr.opcode == TBV_NATIVE_DATA_OP_SEND_IMM) &&
		!memcmp(credit->src_host, m->src_host, 16) &&
		!memcmp(credit->dst_host, m->dst_host, 16) &&
		m->hdr.src_qp == credit->hdr.src_qp &&
		m->hdr.dest_qp == credit->hdr.dest_qp &&
		m->hdr.psn == credit->hdr.psn &&
		m->hdr.length == credit->hdr.length && !m->offset &&
		m->bytes == credit->bytes && m->count == credit->count &&
		!memcmp(m->lanes, credit->lanes, sizeof(m->lanes));
}

/* Replay watermark for a new WR from the sending host (state->lock). */
static bool tbv_dmabuf_token_fresh(struct tbv_state *state,
				   const struct tbv_dmabuf_wire *m, bool update)
{
	struct tbv_peer *peer;
	bool fresh = true;

	mutex_lock(&state->lock);
	list_for_each_entry(peer, &state->peers, node) {
		if (!peer->remote_host_known ||
		    memcmp(peer->remote_host_id, m->src_host, 16))
			continue;
		if (m->token <= peer->dmabuf_rx_token)
			fresh = false;
		else if (update)
			peer->dmabuf_rx_token = m->token;
	}
	mutex_unlock(&state->lock);
	return fresh;
}

/* Turn the armed credit into the WR that arrived for it. */
static void tbv_dmabuf_spec_adopt(const struct tbv_dmabuf_wire *m, int deferred)
{
	struct tbv_dmabuf_rx *rx = &dmabuf_engine.rx;

	tbv_dmabuf_token_fresh(dmabuf_engine.state, m, true);
	rx->request = *m;
	rx->request.op = TBV_DMABUF_PREPARE;
	rx->request.credit = 0;
	rx->request.status = 0;
	rx->deferred = deferred;
	rx->adopted_credit = 0;
	rx->next_offset = 0;
	rx->deadline = jiffies + msecs_to_jiffies(30000);
	tbv_control_put(rx->credit_ctrl);
	rx->credit_ctrl = NULL;
	rx->cancel_deadline = 0;
	rx->state = TBV_DMABUF_RX_ARMED;
	wake_up_all(&dmabuf_engine.rx_wait);
}

static void tbv_dmabuf_spec_drop(bool abort)
{
	struct tbv_dmabuf_rx *rx = &dmabuf_engine.rx;

	tbv_dmabuf_batch_drop(&rx->batch, abort);
	if (rx->buffer.finish)
		rx->buffer.finish(rx->buffer.context, -ECANCELED);
	memset(&rx->buffer, 0, sizeof(rx->buffer));
	tbv_control_put(rx->credit_ctrl);
	rx->credit_ctrl = NULL;
	rx->request = rx->last;
	rx->state = rx->last_valid ? TBV_DMABUF_RX_CACHED : TBV_DMABUF_RX_IDLE;
	tbv_dmabuf_credit_count(abort ? CREDIT_ABORTED : CREDIT_RELEASED);
	wake_up_all(&dmabuf_engine.rx_wait);
}

/* Ask the sender to fill the armed pages; it acknowledges with RELEASE. */
static void tbv_dmabuf_spec_cancel(void)
{
	struct tbv_dmabuf_rx *rx = &dmabuf_engine.rx;
	struct tbv_dmabuf_wire m = rx->request;

	if (rx->cancel_sent || rx->released)
		return;
	rx->cancel_sent = true;
	rx->release_deadline = jiffies + msecs_to_jiffies(500);
	m.op = TBV_DMABUF_CREDIT | TBV_DMABUF_REPLY;
	m.status = -ECANCELED;
	tbv_dmabuf_credit_count(CREDIT_CANCELED);
	if (tbv_control_notify(rx->credit_ctrl, &m)) {
		tbv_dmabuf_spec_drop(true);
		return;
	}
	mod_delayed_work(system_unbound_wq, &dmabuf_engine.rx_watchdog,
			 msecs_to_jiffies(10));
}

/* Returns true while the credit is still armed and unresolved. */
static bool tbv_dmabuf_spec_poll(void)
{
	struct tbv_dmabuf_rx *rx = &dmabuf_engine.rx;

	if (rx->state != TBV_DMABUF_RX_SPECULATIVE)
		return false;
	if (atomic_read(&rx->batch.error)) {
		tbv_dmabuf_spec_drop(true);
		return false;
	}
	if (rx->released && !atomic_read(&rx->batch.pending)) {
		dma_rmb();
		tbv_dmabuf_spec_drop(false);
		return false;
	}
	if (!rx->buffer.alive(rx->buffer.context))
		tbv_dmabuf_spec_cancel();
	if (rx->state == TBV_DMABUF_RX_SPECULATIVE && rx->cancel_sent &&
	    time_after_eq(jiffies, rx->release_deadline))
		tbv_dmabuf_spec_drop(true);
	return rx->state == TBV_DMABUF_RX_SPECULATIVE;
}

/* Arm the remembered stream's head WQE and grant its sender a credit. */
static void tbv_dmabuf_credit_arm(void)
{
	struct tbv_dmabuf_rx *rx = &dmabuf_engine.rx;
	struct tbv_dmabuf_wire m = dmabuf_engine.want;
	struct tbv_control *ctrl = dmabuf_engine.want_ctrl;
	struct tbv_dmabuf_buffer buffer = {};
	u64 perf_start = tbv_perf_clock();
	u32 i, j;
	int ret;

	if (!ctrl || tbv_dmabuf_rx_active(rx) ||
	    rx->state == TBV_DMABUF_RX_SPECULATIVE)
		return;
	if (!tbv_control_credits(ctrl)) {
		tbv_dmabuf_want_clear();
		return;
	}
	memset(&rx->batch, 0, sizeof(rx->batch));
	for (i = 0; i < m.count; i++) {
		struct tbv_rail *rail = tbv_dmabuf_find_lane(dmabuf_engine.state, &m, i);

		if (!rail)
			goto forget;
		for (j = 0; j < i; j++)
			if (rx->batch.lanes[j].rail == rail)
				break;
		rx->batch.lanes[rx->batch.count++].rail = rail;
		if (j != i)
			goto forget;
	}
	m.hdr.imm_data = 0;
	ret = tbv_ibdev_dmabuf_peek(dmabuf_engine.state, rx->batch.lanes[0].rail,
				   &m.hdr, &buffer);
	if (ret == -EAGAIN) {
		/* Retried from post_recv. Drop lanes; the stream stays wanted. */
		dmabuf_engine.want_owner = buffer.owner;
		tbv_dmabuf_batch_drop(&rx->batch, false);
		return;
	}
	if (ret)
		goto forget;
	if (!++dmabuf_engine.credit_seq)
		dmabuf_engine.credit_seq++;
	m.op = TBV_DMABUF_CREDIT;
	m.credit = dmabuf_engine.credit_seq;
	m.status = 0;
	rx->request = m;
	rx->buffer = buffer;
	ret = tbv_dmabuf_batch_alloc(&rx->batch, &m, &rx->buffer, false);
	if (!ret)
		ret = tbv_dmabuf_batch_post(&rx->batch, false);
	rx->state = TBV_DMABUF_RX_SPECULATIVE;
	rx->released = false;
	rx->cancel_sent = false;
	rx->credit_ctrl = ctrl;
	dmabuf_engine.want_ctrl = NULL;
	dmabuf_engine.want_owner = NULL;
	if (ret) {
		tbv_dmabuf_spec_drop(true);
		return;
	}
	m.op = TBV_DMABUF_CREDIT | TBV_DMABUF_REPLY;
	ret = tbv_control_notify(ctrl, &m);
	tbv_perf_wire(&m, false, "credit_grant", perf_start, ret, m.credit);
	if (ret) {
		/* Nobody can fill descriptors the sender does not know about. */
		tbv_dmabuf_spec_drop(true);
		return;
	}
	tbv_dmabuf_credit_count(CREDIT_GRANTED);
	mod_delayed_work(system_unbound_wq, &dmabuf_engine.rx_watchdog,
			 msecs_to_jiffies(100));
	return;
forget:
	tbv_dmabuf_batch_drop(&rx->batch, false);
	tbv_dmabuf_want_clear();
}

/* After a GPU SEND completed, expect the next one of the same length. */
static void tbv_dmabuf_credit_next(struct tbv_control *ctrl)
{
	const struct tbv_dmabuf_wire *m = &dmabuf_engine.rx.last;

	if (!tbv_control_credits(ctrl) ||
	    (m->hdr.opcode != TBV_NATIVE_DATA_OP_SEND &&
	     m->hdr.opcode != TBV_NATIVE_DATA_OP_SEND_IMM) ||
	    !m->hdr.length || m->offset || m->bytes != m->hdr.length ||
	    m->hdr.length > dmabuf_credit_max)
		return;
	tbv_dmabuf_want_clear();
	dmabuf_engine.want = *m;
	tbv_control_get(ctrl);
	dmabuf_engine.want_ctrl = ctrl;
	tbv_dmabuf_credit_arm();
}

static void tbv_dmabuf_credit_work(struct work_struct *work)
{
	mutex_lock(&dmabuf_engine.rx_lock);
	if (dmabuf_engine.want_owner)
		tbv_dmabuf_credit_arm();
	mutex_unlock(&dmabuf_engine.rx_lock);
}

/* Called from ibv_post_recv(). Not on this CPU's queue: the poster often
 * spins on its CQ next, which delays a per-CPU worker by a time slice. */
void tbv_dmabuf_recv_posted(void *owner)
{
	if (dmabuf_credits && dmabuf_engine.post_wq &&
	    READ_ONCE(dmabuf_engine.want_owner) == owner)
		queue_work(dmabuf_engine.post_wq, &dmabuf_engine.credit_work);
}

/* COMMIT or RELEASE from the data sender for the armed credit. Its frames
 * are already on the way: a credit we do not hold quarantines the lanes. */
static void tbv_dmabuf_credit_commit(struct tbv_peer *peer,
				     const struct tbv_dmabuf_wire *m)
{
	struct tbv_dmabuf_rx *rx = &dmabuf_engine.rx;
	struct tbv_control *ctrl = READ_ONCE(peer->dmabuf_control);
	bool release = m->op == (TBV_DMABUF_RELEASE | TBV_DMABUF_REPLY);
	u64 perf_start = tbv_perf_clock();
	int ret = 0;
	u32 i;

	mutex_lock(&dmabuf_engine.rx_lock);
	if (rx->state != TBV_DMABUF_RX_SPECULATIVE || rx->released ||
	    rx->request.credit != m->credit ||
	    (!release && (!tbv_dmabuf_credit_match(&rx->request, m) ||
			  !tbv_dmabuf_token_fresh(peer->state, m, false)))) {
		pr_warn_ratelimited("unexpected credit %s %u (armed %u state %d), stopping its rails\n",
				    release ? "release" : "commit", m->credit,
				    rx->state == TBV_DMABUF_RX_SPECULATIVE ? rx->request.credit : 0,
				    rx->state);
		tbv_dmabuf_credit_count(CREDIT_MISMATCH);
		for (i = 0; i < m->count; i++) {
			struct tbv_rail *rail = tbv_dmabuf_find_lane(peer->state, m, i);

			if (rail) {
				tbv_dmabuf_stop_rail(rail);
				tbv_rail_put(rail);
			}
		}
		if (rx->state == TBV_DMABUF_RX_SPECULATIVE)
			tbv_dmabuf_spec_drop(true);
		goto out;
	}
	if (release) {
		rx->released = true;
		tbv_dmabuf_spec_poll();
		goto out;
	}
	ret = tbv_ibdev_dmabuf_claim(&rx->buffer, &m->hdr);
	tbv_dmabuf_spec_adopt(m, ret);
	if (ctrl)
		tbv_control_get(ctrl);
	rx->control = ctrl;
	tbv_dmabuf_credit_count(CREDIT_COMMITTED);
	if (!atomic_read(&rx->batch.pending) || atomic_read(&rx->batch.error))
		tbv_dmabuf_rx_reap();
	else
		mod_delayed_work(system_unbound_wq, &dmabuf_engine.rx_watchdog,
				 msecs_to_jiffies(10));
out:
	mutex_unlock(&dmabuf_engine.rx_lock);
	tbv_perf_wire(m, false, release ? "credit_release_rx" : "credit_commit_rx",
		      perf_start, ret, m->credit);
}

/* The PREPARE matches the armed credit: a credit was in flight when its
 * sender started. READY names the credit, so the sender drops it. */
static int tbv_dmabuf_spec_prepare(struct tbv_state *state,
				   struct tbv_dmabuf_wire *m)
{
	struct tbv_dmabuf_rx *rx = &dmabuf_engine.rx;
	u32 credit = rx->request.credit;

	if (rx->released) {
		/* The filler frames follow RELEASE closely; settle in place. */
		wait_event_timeout(rx->batch.wait, !atomic_read(&rx->batch.pending) ||
				   atomic_read(&rx->batch.error), msecs_to_jiffies(2));
		return tbv_dmabuf_spec_poll() ? TBV_DMABUF_BUSY : 0;
	}
	if (rx->cancel_sent || !tbv_dmabuf_credit_match(&rx->request, m) ||
	    !tbv_dmabuf_token_fresh(state, m, false) ||
	    tbv_ibdev_dmabuf_claim(&rx->buffer, &m->hdr)) {
		tbv_dmabuf_spec_cancel();
		return TBV_DMABUF_BUSY;
	}
	tbv_dmabuf_spec_adopt(m, 0);
	rx->adopted_credit = credit;
	m->credit = credit;
	tbv_dmabuf_credit_count(CREDIT_ADOPTED);
	return TBV_DMABUF_READY;
}

static bool tbv_dmabuf_rx_cached(const struct tbv_dmabuf_wire *m)
{
	struct tbv_dmabuf_rx *rx = &dmabuf_engine.rx;

	return !tbv_dmabuf_rx_active(rx) && rx->last_valid &&
		tbv_dmabuf_same(&rx->last, m, true);
}

static int tbv_dmabuf_rx_prepare(struct tbv_state *state,
				 struct tbv_dmabuf_wire *m)
{
	struct tbv_dmabuf_rx *rx = &dmabuf_engine.rx;
	u64 start;
	u64 perf_start = tbv_perf_clock();
	u32 i, j;
	int ret;

	if (tbv_dmabuf_rx_cached(m))
		return rx->last_status ?: TBV_DMABUF_DONE;
	if (rx->state == TBV_DMABUF_RX_SPECULATIVE) {
		ret = m->offset ? TBV_DMABUF_BUSY : tbv_dmabuf_spec_prepare(state, m);
		if (ret == TBV_DMABUF_BUSY && m->offset)
			tbv_dmabuf_spec_cancel();
		if (ret)
			return ret;
	}
	rx->deferred = 0;
	if (tbv_dmabuf_rx_active(rx)) {
		if (!tbv_dmabuf_same(&rx->request, m, false))
			return TBV_DMABUF_BUSY;
		if (rx->state == TBV_DMABUF_RX_ARMED) {
			if (!tbv_dmabuf_same(&rx->request, m, true))
				return -EINVAL;
			/* READY is 0: an adoption above also ends here. */
			if (rx->adopted_credit && m->credit != rx->adopted_credit) {
				m->credit = rx->adopted_credit;
				tbv_dmabuf_credit_count(CREDIT_READOPTED);
			}
			return TBV_DMABUF_READY;
		}
		if (m->offset == rx->request.offset && m->bytes == rx->request.bytes)
			return TBV_DMABUF_DONE;
		if (m->offset != rx->next_offset)
			return -EINVAL;
	} else if (m->offset) {
		return -EINVAL;
	}
	if (!tbv_dmabuf_rx_active(rx)) {
		struct tbv_peer *peer;
		bool stale = false;

		/* Reject old PREPAREs even after QP number/PSN reuse. */
		mutex_lock(&state->lock);
		list_for_each_entry(peer, &state->peers, node)
			if (peer->remote_host_known &&
			    !memcmp(peer->remote_host_id, m->src_host, 16) &&
			    m->token <= peer->dmabuf_rx_token)
				stale = true;
		mutex_unlock(&state->lock);
		if (stale)
			return -ESTALE;
	}
	memset(&rx->batch, 0, sizeof(rx->batch));
	for (i = 0; i < m->count; i++) {
		struct tbv_rail *rail = tbv_dmabuf_find_lane(state, m, i);

		if (!rail) {
			ret = -ENOTCONN;
			goto drop;
		}
		for (j = 0; j < i; j++)
			if (rx->batch.lanes[j].rail == rail)
				break;
		if (j != i) {
			tbv_rail_put(rail);
			ret = -EINVAL;
			goto drop;
		}
		rx->batch.lanes[rx->batch.count++].rail = rail;
	}
	tbv_perf_wire(m, false, "rx_paths", perf_start, 0, m->count);
	if (!tbv_dmabuf_rx_active(rx)) {
		perf_start = tbv_perf_clock();
		ret = tbv_ibdev_dmabuf_begin(state, rx->batch.lanes[0].rail,
					     &m->hdr, &rx->buffer);
		tbv_perf_wire(m, false, "rx_verbs_begin", perf_start, ret, 0);
		if (ret)
			goto drop;
		rx->state = TBV_DMABUF_RX_OPEN;
		rx->adopted_credit = 0;
		mutex_lock(&state->lock);
		{
			struct tbv_peer *peer;

			list_for_each_entry(peer, &state->peers, node)
				if (peer->remote_host_known &&
				    !memcmp(peer->remote_host_id, m->src_host, 16))
					peer->dmabuf_rx_token = m->token;
		}
		mutex_unlock(&state->lock);
	}
	rx->request = *m;
	rx->deadline = jiffies + msecs_to_jiffies(30000);
	start = tbv_dmabuf_clock();
	ret = tbv_dmabuf_batch_alloc(&rx->batch, m, &rx->buffer, false);
	tbv_dmabuf_record(TBV_DMABUF_RX_BUILD, start);
	if (ret)
		goto fail;
	start = tbv_dmabuf_clock();
	perf_start = tbv_perf_clock();
	ret = tbv_dmabuf_batch_post(&rx->batch, false);
	tbv_dmabuf_record(TBV_DMABUF_RX_POST, start);
	tbv_perf_wire(m, false, "batch_post", perf_start, ret, 0);
	if (ret)
		goto fail;
	rx->cancel_deadline = 0;
	rx->state = TBV_DMABUF_RX_ARMED;
	mod_delayed_work(system_unbound_wq, &dmabuf_engine.rx_watchdog,
			 msecs_to_jiffies(10));
	return TBV_DMABUF_READY;
drop:
	tbv_dmabuf_batch_drop(&rx->batch, false);
	return ret;
fail:
	tbv_dmabuf_rx_finish(ret);
	return ret;
}

/*
 * The WR behind an active RX died (QP destroyed or in error). An armed batch's
 * sender was told READY and posts its frames even for a canceled WR, so wait
 * for them as QP destroy does in tbv_dmabuf_cancel_recv: canceling them stops
 * both rails until reload. Nothing is in flight otherwise.
 */
static bool tbv_dmabuf_rx_cancel_due(struct tbv_dmabuf_rx *rx)
{
	if (rx->state != TBV_DMABUF_RX_ARMED || atomic_read(&rx->batch.error))
		return true;
	if (!rx->cancel_deadline)
		rx->cancel_deadline = (jiffies +
			msecs_to_jiffies(TBV_DMABUF_CANCEL_DRAIN_MS)) ?: 1;
	return time_after_eq(jiffies, rx->cancel_deadline);
}

static void tbv_dmabuf_rx_watch(struct work_struct *work)
{
	struct tbv_dmabuf_rx *rx = &dmabuf_engine.rx;

	mutex_lock(&dmabuf_engine.rx_lock);
	if (tbv_dmabuf_rx_active(rx)) {
		if (!rx->buffer.alive(rx->buffer.context) &&
		    tbv_dmabuf_rx_cancel_due(rx))
			tbv_dmabuf_rx_finish(-ECANCELED);
		else if (time_after_eq(jiffies, rx->deadline))
			tbv_dmabuf_rx_finish(-ETIMEDOUT);
		else if (rx->state == TBV_DMABUF_RX_ARMED) {
			tbv_dmabuf_rx_reap();
			if (rx->state == TBV_DMABUF_RX_ARMED)
				tbv_dmabuf_batch_kick(&rx->batch, false);
		}
		if (tbv_dmabuf_rx_active(rx))
			mod_delayed_work(system_unbound_wq, &dmabuf_engine.rx_watchdog,
					 msecs_to_jiffies(10));
	} else if (tbv_dmabuf_spec_poll()) {
		/* An armed credit waits for its WR indefinitely; only a
		 * release in progress needs prompt ring progress. */
		bool settling = rx->released || rx->cancel_sent;

		if (settling)
			tbv_dmabuf_batch_kick(&rx->batch, false);
		mod_delayed_work(system_unbound_wq, &dmabuf_engine.rx_watchdog,
				 msecs_to_jiffies(settling ? 10 : 100));
	}
	mutex_unlock(&dmabuf_engine.rx_lock);
}

int tbv_dmabuf_control_request(struct tbv_peer *peer, struct tbv_control *ctrl,
			       struct tbv_dmabuf_wire *m)
{
	u64 start;
	u64 service;

	if (m->op == TBV_DMABUF_PING) {
		m->status = 0;
		return 0;
	}
	start = tbv_perf_clock();
	mutex_lock(&dmabuf_engine.rx_lock);
	tbv_perf_wire(m, false, "control_rx_lock", start, 0, m->op);
	service = tbv_perf_clock();
	if (m->op == TBV_DMABUF_PREPARE) {
		m->status = tbv_dmabuf_rx_prepare(peer->state, m);
		if (ctrl && m->status == TBV_DMABUF_READY) {
			/* Retried PREPAREs retain the existing tracker. Its RX ref
			 * survives rail release and disconnect on a different link. */
			if (ctrl && m->bytes && !dmabuf_engine.rx.control) {
				tbv_control_get(ctrl);
				dmabuf_engine.rx.control = ctrl;
			}
			/* Empty ownership barriers have no DMA callback. Complete
			 * them in the PREPARE reply, without an extra DONE packet,
			 * with the same QP/CQ checks as a payload WR. */
			if (!atomic_read(&dmabuf_engine.rx.batch.pending))
				m->status = tbv_dmabuf_rx_reap();
		}
	} else if (m->op == TBV_DMABUF_COMPLETE || tbv_dmabuf_credit_op(m->op)) {
		m->status = -ENOENT;
	} else if (tbv_dmabuf_rx_cached(m)) {
		m->status = m->op == TBV_DMABUF_ABORT ? -ECANCELED :
			    dmabuf_engine.rx.last_status ?: TBV_DMABUF_DONE;
	} else if (m->op == TBV_DMABUF_STATUS &&
		   dmabuf_engine.rx.state == TBV_DMABUF_RX_SPECULATIVE &&
		   !dmabuf_engine.rx.released && !dmabuf_engine.rx.cancel_sent &&
		   tbv_dmabuf_credit_match(&dmabuf_engine.rx.request, m)) {
		/* The sender used the armed credit and its frames may already be
		 * placed, but COMMIT is still in flight. Not an unknown WR: let
		 * the sender keep waiting instead of failing it and its rails. */
		m->status = TBV_DMABUF_BUSY;
	} else if (!tbv_dmabuf_rx_active(&dmabuf_engine.rx) ||
		   !tbv_dmabuf_same(&dmabuf_engine.rx.request, m, true)) {
		m->status = -ENOENT;
	} else if (m->op == TBV_DMABUF_ABORT) {
		tbv_dmabuf_rx_finish(-ECANCELED);
		m->status = -ECANCELED;
	} else {
		m->status = tbv_dmabuf_rx_reap();
	}
	mutex_unlock(&dmabuf_engine.rx_lock);
	tbv_perf_wire(m, false, "control_service", service, m->status, m->op);
	return m->status;
}

/* COMMIT/RELEASE arrive at the data receiver, oriented like a PREPARE. */
static void tbv_dmabuf_credit_notified(struct tbv_state *state,
				       struct tb_xdomain *source,
				       const struct tbv_dmabuf_wire *m)
{
	struct tbv_peer *peer, *found = NULL;

	if (m->status || memcmp(m->dst_host, state->native_host_id, 16))
		return;
	mutex_lock(&state->lock);
	list_for_each_entry(peer, &state->peers, node) {
		if ((source && peer->xd != source) ||
		    !peer->remote_host_known || !peer->xd->remote_uuid ||
		    memcmp(peer->remote_host_id, m->src_host, 16) ||
		    memcmp(peer->xd->remote_uuid, m->src_domain, 16))
			continue;
		refcount_inc(&peer->refcnt);
		found = peer;
		break;
	}
	mutex_unlock(&state->lock);
	if (!found)
		return;
	if (tbv_control_credits(READ_ONCE(found->dmabuf_control)))
		tbv_dmabuf_credit_commit(found, m);
	tbv_peer_put(state, found);
}

int tbv_dmabuf_handle_packet(struct tbv_state *state,
			     struct tb_xdomain *source, const void *buf, size_t size)
{
	struct tbv_dmabuf_wire m;
	struct tbv_peer *peer, *found = NULL;
	struct tb_xdomain *xd = NULL;
	u8 response[TBV_DMABUF_WIRE_SIZE];
	struct tbv_control *ctrl = NULL;
	u64 perf_start = tbv_perf_clock();
	int ret;

	/* Consume DONE before core matching, including malformed, late, or
	 * disabled-mode notifications. Only fully validated replies wake TX. */
	if (tbv_dmabuf_wire_notification(buf, size)) {
		if (!dmabuf_verbs || tbv_dmabuf_wire_parse(buf, size, &m))
			return 1;
		if (m.op == (TBV_DMABUF_COMPLETE | TBV_DMABUF_REPLY) ||
		    m.op == (TBV_DMABUF_CREDIT | TBV_DMABUF_REPLY))
			tbv_control_handle_done(state, source, &m);
		else if (m.op == (TBV_DMABUF_COMMIT | TBV_DMABUF_REPLY) ||
			 m.op == (TBV_DMABUF_RELEASE | TBV_DMABUF_REPLY))
			tbv_dmabuf_credit_notified(state, source, &m);
		else
			tbv_dmabuf_credit_count(CREDIT_MISMATCH);
		return 1;
	}
	if (!dmabuf_verbs)
		return 0;
	ret = tbv_dmabuf_wire_parse(buf, size, &m);
	if (ret) {
		if (size == TBV_DMABUF_WIRE_SIZE)
			pr_debug("control parse failed: %d\n", ret);
		return 0;
	}
	if (m.op & TBV_DMABUF_REPLY)
		return 0; /* tb_xdomain_request must see its response. */
	if (m.status || memcmp(m.dst_host, state->native_host_id, 16)) {
		pr_debug("control target mismatch token=%llu target=%pUb local=%pUb\n",
			 m.token, m.dst_host, state->native_host_id);
		return 1;
	}
	mutex_lock(&state->lock);
	list_for_each_entry(peer, &state->peers, node) {
		if ((source && peer->xd != source) ||
		    !peer->remote_host_known || !peer->xd->remote_uuid ||
		    memcmp(peer->remote_host_id, m.src_host, 16) ||
		    memcmp(peer->xd->remote_uuid, m.src_domain, 16))
			continue;
		xd = tb_xdomain_get(peer->xd);
		refcount_inc(&peer->refcnt);
		found = peer;
		break;
	}
	mutex_unlock(&state->lock);
	if (!xd) {
		pr_debug("control source mismatch token=%llu host=%pUb domain=%pUb\n",
			 m.token, m.src_host, m.src_domain);
		return 1;
	}
	tbv_perf_wire(&m, false, "control_dispatch", perf_start, 0, m.op);
	if (tbv_control_xdomain_required(found)) {
		ret = tbv_control_setup(found);
		if (!ret)
			ctrl = READ_ONCE(found->dmabuf_control);
		else
			m.status = ret;
	}
	if (!m.status)
		tbv_dmabuf_control_request(found, ctrl, &m);
	pr_debug("control reply op=%u token=%llu status=%d\n", m.op, m.token, m.status);
	m.op |= TBV_DMABUF_REPLY;
	perf_start = tbv_perf_clock();
	ret = tbv_dmabuf_wire_build(response, sizeof(response), &m, xd->route);
	if (ret >= 0)
		ret = tb_xdomain_response(xd, response, sizeof(response), TB_CFG_PKG_XDOMAIN_RESP);
	tbv_perf_wire(&m, false, "control_response", perf_start, ret, m.op);
	tb_xdomain_put(xd);
	tbv_peer_put(state, found);
	return 1;
}

static int tbv_dmabuf_exchange(struct tbv_peer *peer, struct tbv_ctrl_pending *pending,
			       const struct tbv_dmabuf_wire *m, u32 timeout_ms)
{
	u64 start;
	u64 perf_start = tbv_perf_clock();
	int ret;

	start = tbv_dmabuf_clock();
	if (pending) {
		ret = tbv_control_exchange(pending, m, timeout_ms);
	} else {
		ret = tbv_control_xdomain_exchange(peer, m, timeout_ms);
	}
	if (m->op == TBV_DMABUF_PREPARE || m->op == TBV_DMABUF_STATUS)
		tbv_dmabuf_record(m->op == TBV_DMABUF_PREPARE ?
			TBV_DMABUF_PREPARE_TIME : TBV_DMABUF_STATUS_TIME, start);
	if (ret < 0) {
		pr_debug("control exchange op=%u token=%llu transport=%d\n",
			 m->op, m->token, ret);
	}
	tbv_perf_wire(m, true, "control_exchange", perf_start, ret, m->op);
	return ret;
}

static int tbv_dmabuf_tx_paths(struct tbv_dmabuf_job *job,
			       struct tbv_dmabuf_wire *m,
			       struct tbv_dmabuf_batch *batch)
{
	struct tbv_state *state = job->state;
	struct tbv_peer *peer;

	mutex_lock(&state->lock);
	list_for_each_entry(peer, &state->peers, node) {
		struct tbv_rail *rail;

		if (!peer->remote_host_known ||
		    memcmp(peer->remote_host_id, job->route->peer->remote_host_id, 16))
			continue;
		list_for_each_entry(rail, &peer->rails, node) {
			u32 n = batch->count;

			if (n == TBV_DMABUF_MAX_RAILS || !tbv_dmabuf_rail_ready(rail) ||
			    !peer->xd->local_uuid)
				continue;
			refcount_inc(&rail->refcnt);
			batch->lanes[batch->count++].rail = rail;
			memcpy(m->lanes[n].domain, peer->xd->local_uuid, 16);
			m->lanes[n].path = rail->path.local_transmit_path;
		}
	}
	m->count = batch->count;
	mutex_unlock(&state->lock);
	return batch->count ? 0 : -ENOTCONN;
}

/* Sender side. Credits arrive on any link to the receiving host; done
 * guards against duplicates, credits seen adopted, and stale cancels. */
static struct tbv_dmabuf_credit *tbv_dmabuf_credit_slot(const u8 *host, bool create)
{
	struct tbv_dmabuf_credit *c;
	u32 i;

	lockdep_assert_held(&dmabuf_engine.credit_lock);
	for (i = 0; i < TBV_DMABUF_CREDIT_HOSTS; i++) {
		c = &dmabuf_engine.credits[i];
		if (c->used && !memcmp(c->host, host, 16))
			return c;
	}
	if (!create)
		return NULL;
	for (i = 0; i < TBV_DMABUF_CREDIT_HOSTS; i++)
		if (!dmabuf_engine.credits[i].used)
			break;
	if (i == TBV_DMABUF_CREDIT_HOSTS)
		i = dmabuf_engine.credit_victim++ % TBV_DMABUF_CREDIT_HOSTS;
	c = &dmabuf_engine.credits[i];
	memset(c, 0, sizeof(*c));
	memcpy(c->host, host, 16);
	c->used = true;
	return c;
}

void tbv_dmabuf_credit_received(struct tbv_peer *peer,
				const struct tbv_dmabuf_wire *m)
{
	struct tbv_dmabuf_credit *c;
	unsigned long flags;
	bool settle = false;

	if (!tbv_dmabuf_credits_enabled() || !READ_ONCE(peer->remote_dmabuf_credit))
		return;
	spin_lock_irqsave(&dmabuf_engine.credit_lock, flags);
	c = tbv_dmabuf_credit_slot(m->dst_host, true);
	if (m->credit <= c->done) {
		tbv_dmabuf_credit_count(CREDIT_STALE);
	} else {
		if (!c->valid || c->wire.credit < m->credit) {
			c->wire = *m;
			c->wire.status = 0;
			c->valid = true;
			c->cancel = false;
		}
		if (m->status < 0 && c->wire.credit == m->credit) {
			c->cancel = true;
			settle = true;
		}
	}
	spin_unlock_irqrestore(&dmabuf_engine.credit_lock, flags);
	tbv_perf_wire_mark(m, true, "credit_received", m->status, m->credit);
	wake_up_all(&dmabuf_engine.credit_wait);
	if (settle && dmabuf_engine.tx_wq)
		queue_work(dmabuf_engine.tx_wq, &dmabuf_engine.settle_work);
}

static bool tbv_dmabuf_credit_held(const u8 *host)
{
	struct tbv_dmabuf_credit *c;
	unsigned long flags;
	bool held;

	spin_lock_irqsave(&dmabuf_engine.credit_lock, flags);
	c = tbv_dmabuf_credit_slot(host, false);
	held = c && c->valid;
	spin_unlock_irqrestore(&dmabuf_engine.credit_lock, flags);
	return held;
}

/* READY adopted this credit for the PREPARE that raced it. */
static void tbv_dmabuf_credit_adopted(const u8 *host, u32 credit)
{
	struct tbv_dmabuf_credit *c;
	unsigned long flags;

	spin_lock_irqsave(&dmabuf_engine.credit_lock, flags);
	c = tbv_dmabuf_credit_slot(host, true);
	c->done = max(c->done, credit);
	if (c->valid && c->wire.credit <= credit)
		c->valid = false;
	spin_unlock_irqrestore(&dmabuf_engine.credit_lock, flags);
}

/* Takes a held credit that matches this WR, without releasing any other. */
static u32 tbv_dmabuf_credit_take(const u8 *host, const struct tbv_dmabuf_wire *m)
{
	struct tbv_dmabuf_credit *c;
	unsigned long flags;
	u32 credit = 0;

	spin_lock_irqsave(&dmabuf_engine.credit_lock, flags);
	c = tbv_dmabuf_credit_slot(host, false);
	if (c && c->valid && !c->cancel && tbv_dmabuf_credit_match(&c->wire, m)) {
		credit = c->wire.credit;
		c->valid = false;
		c->done = max(c->done, credit);
	}
	spin_unlock_irqrestore(&dmabuf_engine.credit_lock, flags);
	if (credit)
		tbv_dmabuf_credit_count(CREDIT_USED);
	return credit;
}

static int tbv_dmabuf_zero_copy(void *context, u32 offset, void *data,
				u32 bytes, bool to_mr)
{
	memset(data, 0, bytes);
	return 0;
}

static bool tbv_dmabuf_always_alive(void *context)
{
	return true;
}

/* Fill an unusable credit's armed pages, which the receiver still owns as
 * a posted WQE, with exactly the frames it expects; then say RELEASE. */
static void tbv_dmabuf_credit_release(struct tbv_state *state,
				      const struct tbv_dmabuf_wire *credit)
{
	struct tbv_dmabuf_wire m = *credit;
	struct tbv_dmabuf_batch batch = {};
	struct tbv_dmabuf_buffer buffer = {
		.uncached = true,
		.copy = tbv_dmabuf_zero_copy,
		.alive = tbv_dmabuf_always_alive,
	};
	struct tbv_control *ctrl = NULL;
	struct tbv_peer *peer;
	u64 perf_start = tbv_perf_clock();
	u32 i;
	int ret = 0;

	mutex_lock(&state->lock);
	for (i = 0; i < m.count && !ret; i++) {
		struct tbv_rail *found = NULL;

		list_for_each_entry(peer, &state->peers, node) {
			struct tbv_rail *rail;

			if (!peer->remote_host_known || !peer->xd->local_uuid ||
			    memcmp(peer->remote_host_id, m.dst_host, 16) ||
			    memcmp(peer->xd->local_uuid, m.lanes[i].domain, 16))
				continue;
			list_for_each_entry(rail, &peer->rails, node)
				if (tbv_dmabuf_rail_ready(rail) &&
				    rail->path.local_transmit_path == m.lanes[i].path)
					found = rail;
		}
		if (found) {
			refcount_inc(&found->refcnt);
			batch.lanes[batch.count++].rail = found;
		} else {
			ret = -ENOTCONN;
		}
	}
	list_for_each_entry(peer, &state->peers, node)
		if (!ctrl && peer->remote_host_known && peer->xd->local_uuid &&
		    !memcmp(peer->remote_host_id, m.dst_host, 16) &&
		    !memcmp(peer->xd->local_uuid, m.src_domain, 16) &&
		    tbv_control_credits(READ_ONCE(peer->dmabuf_control))) {
			ctrl = READ_ONCE(peer->dmabuf_control);
			tbv_control_get(ctrl);
		}
	mutex_unlock(&state->lock);
	if (!ret && !ctrl)
		ret = -ENOTCONN;
	if (!ret)
		ret = tbv_dmabuf_batch_alloc(&batch, &m, &buffer, true);
	if (!ret) {
		dma_wmb();
		ret = tbv_dmabuf_batch_post(&batch, true);
	}
	if (!ret)
		ret = tbv_dmabuf_batch_wait(&batch, &buffer,
					    jiffies + msecs_to_jiffies(1000));
	if (!ret) {
		m.op = TBV_DMABUF_RELEASE | TBV_DMABUF_REPLY;
		m.status = 0;
		ret = tbv_control_notify(ctrl, &m);
	}
	/* Without RELEASE the receiver quarantines its rails after 500 ms. */
	tbv_dmabuf_batch_drop(&batch, ret && batch.submitted);
	tbv_control_put(ctrl);
	tbv_dmabuf_credit_count(CREDIT_RETURNED);
	tbv_perf_wire(&m, true, "credit_release", perf_start, ret, m.credit);
	if (ret)
		pr_warn_ratelimited("credit %u release failed: %d\n", m.credit, ret);
}

/*
 * Runs on the ordered TX queue only. Returns the credit this WR may use
 * (m NULL: none), after filling any other credit held for its host.
 */
static u32 tbv_dmabuf_credit_settle(struct tbv_state *state, const u8 *host,
				    const struct tbv_dmabuf_wire *m)
{
	struct tbv_dmabuf_credit *c;
	struct tbv_dmabuf_wire credit;
	unsigned long flags;

	spin_lock_irqsave(&dmabuf_engine.credit_lock, flags);
	c = tbv_dmabuf_credit_slot(host, false);
	if (!c || !c->valid) {
		spin_unlock_irqrestore(&dmabuf_engine.credit_lock, flags);
		return 0;
	}
	credit = c->wire;
	c->valid = false;
	c->done = max(c->done, credit.credit);
	if (m && !c->cancel && tbv_dmabuf_credit_match(&credit, m)) {
		spin_unlock_irqrestore(&dmabuf_engine.credit_lock, flags);
		tbv_dmabuf_credit_count(CREDIT_USED);
		return credit.credit;
	}
	spin_unlock_irqrestore(&dmabuf_engine.credit_lock, flags);
	tbv_dmabuf_credit_release(state, &credit);
	return 0;
}

static void tbv_dmabuf_settle_work(struct work_struct *work)
{
	u8 hosts[TBV_DMABUF_CREDIT_HOSTS][16];
	unsigned long flags;
	u32 i, n = 0;

	spin_lock_irqsave(&dmabuf_engine.credit_lock, flags);
	for (i = 0; i < TBV_DMABUF_CREDIT_HOSTS; i++)
		if (dmabuf_engine.credits[i].valid && dmabuf_engine.credits[i].cancel)
			memcpy(hosts[n++], dmabuf_engine.credits[i].host, 16);
	spin_unlock_irqrestore(&dmabuf_engine.credit_lock, flags);
	for (i = 0; i < n; i++)
		tbv_dmabuf_credit_settle(dmabuf_engine.state, hosts[i], NULL);
}

static int tbv_dmabuf_tx(struct tbv_dmabuf_job *job)
{
	struct tbv_dmabuf_wire m = {};
	struct tbv_dmabuf_batch *batch = &job->batch;
	struct tbv_peer *peer = job->route->peer;
	struct tbv_ctrl_pending *pending = NULL;
	bool fast = tbv_control_required(peer);
	bool credits = fast && tbv_dmabuf_credits_enabled() &&
		READ_ONCE(peer->remote_dmabuf_credit) &&
		tbv_control_xdomain_required(peer);
	u32 offset = 0;
	int ret = 0;

	if (job->posted) {
		m = job->wire;
		pending = job->pending;
		goto prepared;
	}
	m.token = atomic64_inc_return(&dmabuf_engine.token);
	if (!m.token)
		return -EOVERFLOW;
	m.hdr = job->hdr;
	if (job->perf_queued_ns) {
		job->perf = tbv_perf_wire_id(&m, true);
		job->perf.wr_id = job->buffer.wr_id;
		job->perf.bytes = job->hdr.length;
	}
	m.hdr.flags |= TBV_NATIVE_DATA_F_RAW_STREAM;
	memcpy(m.src_host, job->state->native_host_id, 16);
	memcpy(m.dst_host, peer->remote_host_id, 16);
	memcpy(m.src_domain, peer->xd->local_uuid, 16);
prepared:
	do {
		u64 start;
		u64 perf_start, batch_start = tbv_perf_clock();
		unsigned long deadline = jiffies + job->timeout * (job->retries + 1);
		u32 rnr = 0, failures = 0, credit = 0;
		bool remote_armed = false, canceled = false;

		if (job->posted) {
			/* tbv_dmabuf_tx_direct() sent COMMIT and posted the frames. */
			job->posted = false;
			deadline = job->deadline;
			remote_armed = true;
			ret = job->direct_ret;
			goto posted;
		}
		memset(m.lanes, 0, sizeof(m.lanes));
		perf_start = tbv_perf_clock();
		mutex_lock(&dmabuf_engine.tx_lock);
		dmabuf_engine.active_context = job->buffer.context;
		dmabuf_engine.active_batch = batch;
		ret = tbv_dmabuf_tx_paths(job, &m, batch);
		tbv_perf_span(&job->perf, "tx_paths_lock", perf_start, ret, m.count);
		if (ret) {
			mutex_unlock(&dmabuf_engine.tx_lock);
			goto drop;
		}
		m.offset = offset;
		m.bytes = min_t(u32, job->hdr.length - offset,
			m.count * TBV_DMABUF_BATCH_FRAMES * TBV_DMABUF_FRAME_BYTES);
		start = tbv_dmabuf_clock();
		ret = tbv_dmabuf_batch_alloc(batch, &m, &job->buffer, true);
		tbv_dmabuf_record(TBV_DMABUF_TX_BUILD, start);
		mutex_unlock(&dmabuf_engine.tx_lock);
		if (ret)
			goto drop;
		/* Fill any credit this WR cannot use before PREPARE can reach
		 * the receiver; a matching one replaces the PREPARE exchange. */
		if (credits)
			credit = tbv_dmabuf_credit_settle(job->state, m.dst_host,
							  offset ? NULL : &m);
		m.op = TBV_DMABUF_PREPARE;
		if (fast) {
			pending = tbv_control_open(peer, &m);
			if (IS_ERR(pending)) {
				ret = PTR_ERR(pending);
				pending = NULL;
				goto drop;
			}
		}
		for (;;) {
			if (credit) {
				/* DONE is registered first; frames may precede COMMIT. */
				m.op = TBV_DMABUF_COMMIT | TBV_DMABUF_REPLY;
				m.credit = credit;
				perf_start = tbv_perf_clock();
				ret = tbv_control_notify(tbv_control_of(pending), &m);
				tbv_perf_wire(&m, true, "credit_commit", perf_start, ret, credit);
				m.op = TBV_DMABUF_PREPARE;
				m.credit = 0;
				if (ret)
					goto drop;
				ret = TBV_DMABUF_READY;
				remote_armed = true;
				break;
			}
			if (READ_ONCE(batch->canceled) ||
			    !job->buffer.alive(job->buffer.context)) {
				ret = -ECANCELED;
				break;
			}
			ret = tbv_dmabuf_exchange(peer, pending, &m,
				clamp_t(u32, jiffies_to_msecs(job->timeout), 10, 1000));
			if (ret == TBV_DMABUF_READY) {
				if (tbv_control_reply_credit(pending))
					tbv_dmabuf_credit_adopted(m.dst_host,
						tbv_control_reply_credit(pending));
				remote_armed = true;
				break;
			}
			/* The receiver holds a credit for us: use or fill it. */
			if (ret == TBV_DMABUF_BUSY && credits) {
				credit = tbv_dmabuf_credit_settle(job->state, m.dst_host,
								  offset ? NULL : &m);
				if (credit)
					continue;
			}
			if (ret == TBV_DMABUF_DONE)
				break;
			if (ret == TBV_DMABUF_RNR) {
				if (job->rnr_retries != U8_MAX && rnr++ >= job->rnr_retries) {
					ret = -EAGAIN;
					break;
				}
				deadline = jiffies + job->timeout * (job->retries + 1);
			} else if (ret != TBV_DMABUF_BUSY &&
				   (ret != -ETIMEDOUT || failures++ >= job->retries)) {
				break;
			}
			if (time_after_eq(jiffies, deadline)) {
				ret = -ETIMEDOUT;
				break;
			}
			start = tbv_dmabuf_clock();
			perf_start = tbv_perf_clock();
			if (ret == TBV_DMABUF_RNR && credits && !offset) {
				/* The receiver grants a credit once it posts the WQE
				 * this SEND needs: wait for that instead of polling. */
				wait_event_timeout(dmabuf_engine.credit_wait,
					tbv_dmabuf_credit_held(m.dst_host), msecs_to_jiffies(2));
				tbv_dmabuf_record(TBV_DMABUF_RNR_WAIT, start);
				tbv_perf_wire(&m, true, "rnr_credit_wait", perf_start, ret, rnr);
				credit = tbv_dmabuf_credit_settle(job->state, m.dst_host, &m);
				if (credit)
					rnr--;
				continue;
			}
			usleep_range(1000, 2000);
			tbv_dmabuf_record(ret == TBV_DMABUF_RNR ? TBV_DMABUF_RNR_WAIT :
				TBV_DMABUF_PREPARE_WAIT, start);
			tbv_perf_wire(&m, true, "prepare_retry_sleep", perf_start, ret, rnr);
		}
		if (ret == TBV_DMABUF_DONE) {
			tbv_control_close(pending);
			pending = NULL;
			mutex_lock(&dmabuf_engine.tx_lock);
			dmabuf_engine.active_context = NULL;
			dmabuf_engine.active_batch = NULL;
			tbv_dmabuf_batch_drop(batch, false);
			mutex_unlock(&dmabuf_engine.tx_lock);
			tbv_perf_wire(&m, true, "batch", batch_start, 0, 0);
			offset += m.bytes;
			continue;
		}
		if (ret)
			goto abort;
		dma_wmb();
		start = tbv_dmabuf_clock();
		perf_start = tbv_perf_clock();
		/* The receiver is armed for these frames (READY or a credit's
		 * COMMIT). Post them even for a canceled WR: ABORT would leave
		 * its descriptors armed, and it stops its rails to reclaim them.
		 * QP destroy waits for this worker, so the buffer stays valid;
		 * the wait below bounds a canceled batch to the drain time. */
		mutex_lock(&dmabuf_engine.tx_lock);
		ret = tbv_dmabuf_batch_post(batch, true);
		mutex_unlock(&dmabuf_engine.tx_lock);
		tbv_dmabuf_record(TBV_DMABUF_TX_POST, start);
		tbv_perf_wire(&m, true, "batch_post", perf_start, ret, 0);
posted:
		if (!ret) {
			start = tbv_dmabuf_clock();
			perf_start = tbv_perf_clock();
			ret = READ_ONCE(batch->canceled) ? -ECANCELED :
				tbv_dmabuf_batch_wait(batch, &job->buffer, deadline);
			if (ret == -ECANCELED) {
				/* The WR is gone but its frames are posted to an
				 * armed receiver: finish the batch, as far as
				 * DONE, instead of aborting it and its rails. */
				unsigned long drain = jiffies +
					msecs_to_jiffies(TBV_DMABUF_CANCEL_DRAIN_MS);

				canceled = true;
				if (time_before(drain, deadline))
					deadline = drain;
				ret = tbv_dmabuf_batch_drain(batch, deadline) ? 0 :
					atomic_read(&batch->error) ?: -ETIMEDOUT;
			}
			tbv_dmabuf_record(TBV_DMABUF_TX_WAIT, start);
			tbv_perf_wire(&m, true, "tx_dma_wait", perf_start, ret, 0);
		}
		if (ret)
			goto abort;
		m.op = TBV_DMABUF_STATUS;
		start = tbv_dmabuf_clock();
		for (;;) {
			/* Every frame is sent: a WR canceled now only waits,
			 * briefly, for the receiver to place them. */
			if (!canceled && (READ_ONCE(batch->canceled) ||
					  !job->buffer.alive(job->buffer.context))) {
				unsigned long drain = jiffies +
					msecs_to_jiffies(TBV_DMABUF_CANCEL_DRAIN_MS);

				canceled = true;
				if (time_before(drain, deadline))
					deadline = drain;
			}
			if (pending) {
				/* DONE is registered before PREPARE, so a notification
				 * arriving before local TX completion is retained. Only
				 * probe cached status after a missed 10 ms notification. */
				ret = tbv_control_wait_done(pending, 10);
				if (ret == TBV_DMABUF_BUSY) {
					tbv_perf_wire_mark(&m, true, "status_fallback", ret, 0);
					ret = tbv_dmabuf_exchange(peer, pending, &m,
						clamp_t(u32, jiffies_to_msecs(job->timeout), 10, 1000));
				}
			} else {
				ret = tbv_dmabuf_exchange(peer, NULL, &m,
					clamp_t(u32, jiffies_to_msecs(job->timeout), 10, 1000));
			}
			if (ret == TBV_DMABUF_DONE) {
				ret = 0;
				break;
			}
			if (ret != TBV_DMABUF_BUSY && ret != -ETIMEDOUT)
				break;
			if (time_after_eq(jiffies, deadline)) {
				ret = -ETIMEDOUT;
				break;
			}
			if (!pending) {
				u64 sleep_start = tbv_dmabuf_clock();
				u64 perf_sleep = tbv_perf_clock();

				usleep_range(100, 200);
				tbv_dmabuf_record(TBV_DMABUF_STATUS_WAIT, sleep_start);
				tbv_perf_wire(&m, true, "status_retry_sleep", perf_sleep, ret, 0);
			}
		}
		if (pending)
			tbv_dmabuf_record(TBV_DMABUF_DONE_WAIT, start);
abort:
		if (ret && (remote_armed || ret == -ETIMEDOUT || ret == -EPROTO)) {
			m.op = TBV_DMABUF_ABORT;
			tbv_dmabuf_exchange(peer, pending, &m, 1000);
		}
drop:
		tbv_control_close(pending);
		pending = NULL;
		mutex_lock(&dmabuf_engine.tx_lock);
		dmabuf_engine.active_context = NULL;
		dmabuf_engine.active_batch = NULL;
		tbv_dmabuf_batch_drop(batch, !!ret);
		mutex_unlock(&dmabuf_engine.tx_lock);
		tbv_perf_wire(&m, true, "batch", batch_start, ret, 0);
		if (canceled)
			atomic64_inc(ret ? &dmabuf_cancel_stopped : &dmabuf_cancel_drained);
		if (ret)
			return ret;
		if (canceled)
			return -ECANCELED;
		offset += m.bytes;
	} while (offset < job->hdr.length);
	return 0;
}

/*
 * ibv_post_send() context, with no other TX job queued or running: send COMMIT
 * and post the frames of a single-batch GPU SEND that a held credit covers,
 * saving the TX worker's wakeup before them. The worker then only waits for
 * the DMA and DONE and finishes the WR. Anything else stays with the worker.
 */
static void tbv_dmabuf_tx_direct(struct tbv_dmabuf_job *job)
{
	struct tbv_peer *peer = job->route->peer;
	struct tbv_dmabuf_wire *m = &job->wire;
	struct tbv_dmabuf_batch *batch = &job->batch;
	struct tbv_ctrl_pending *pending;
	u64 perf_start = tbv_perf_clock();
	u32 credit;
	int ret;

	if (!tbv_dmabuf_credits_enabled() || !READ_ONCE(peer->remote_dmabuf_credit) ||
	    !tbv_control_xdomain_required(peer) ||
	    (job->hdr.opcode != TBV_NATIVE_DATA_OP_SEND &&
	     job->hdr.opcode != TBV_NATIVE_DATA_OP_SEND_IMM) ||
	    !job->hdr.length || job->hdr.length > dmabuf_credit_max ||
	    !tbv_dmabuf_credit_held(peer->remote_host_id))
		return;
	memset(m, 0, sizeof(*m));
	m->token = atomic64_inc_return(&dmabuf_engine.token);
	if (!m->token)
		return;
	m->hdr = job->hdr;
	m->hdr.flags |= TBV_NATIVE_DATA_F_RAW_STREAM;
	memcpy(m->src_host, job->state->native_host_id, 16);
	memcpy(m->dst_host, peer->remote_host_id, 16);
	memcpy(m->src_domain, peer->xd->local_uuid, 16);
	if (job->perf_queued_ns) {
		job->perf = tbv_perf_wire_id(m, true);
		job->perf.wr_id = job->buffer.wr_id;
		job->perf.bytes = job->hdr.length;
	}
	mutex_lock(&dmabuf_engine.tx_lock);
	dmabuf_engine.active_context = job->buffer.context;
	dmabuf_engine.active_batch = batch;
	ret = tbv_dmabuf_tx_paths(job, m, batch);
	if (!ret) {
		m->bytes = m->hdr.length;
		ret = m->bytes > m->count * TBV_DMABUF_BATCH_FRAMES *
			TBV_DMABUF_FRAME_BYTES ? -EMSGSIZE :
			tbv_dmabuf_batch_alloc(batch, m, &job->buffer, true);
	}
	mutex_unlock(&dmabuf_engine.tx_lock);
	if (ret)
		goto drop;
	/* DONE is registered before the credit is taken: a taken credit
	 * must reach the receiver as COMMIT. */
	m->op = TBV_DMABUF_PREPARE;
	pending = tbv_control_open(peer, m);
	if (IS_ERR(pending))
		goto drop;
	credit = tbv_dmabuf_credit_take(m->dst_host, m);
	if (!credit) {
		tbv_control_close(pending);
		goto drop;
	}
	job->pending = pending;
	job->deadline = jiffies + job->timeout * (job->retries + 1);
	job->posted = true;
	m->op = TBV_DMABUF_COMMIT | TBV_DMABUF_REPLY;
	m->credit = credit;
	ret = tbv_control_notify(tbv_control_of(pending), m);
	m->op = TBV_DMABUF_PREPARE;
	m->credit = 0;
	if (!ret) {
		dma_wmb();
		/* COMMIT armed the receiver: post as in tbv_dmabuf_tx(). */
		mutex_lock(&dmabuf_engine.tx_lock);
		ret = tbv_dmabuf_batch_post(batch, true);
		mutex_unlock(&dmabuf_engine.tx_lock);
	}
	job->direct_ret = ret;
	if (!ret)
		tbv_dmabuf_credit_count(CREDIT_DIRECT);
	tbv_perf_wire(m, true, "tx_direct", perf_start, ret, credit);
	return;
drop:
	mutex_lock(&dmabuf_engine.tx_lock);
	dmabuf_engine.active_context = NULL;
	dmabuf_engine.active_batch = NULL;
	tbv_dmabuf_batch_drop(batch, false);
	mutex_unlock(&dmabuf_engine.tx_lock);
}

static void tbv_dmabuf_tx_work(struct work_struct *work)
{
	struct tbv_dmabuf_job *job = container_of(work, struct tbv_dmabuf_job, work);
	u64 start = tbv_dmabuf_clock();
	u64 finish;
	u64 perf_start = tbv_perf_clock(), perf_end, perf_finish;
	int ret;

	tbv_dmabuf_record(TBV_DMABUF_QUEUE, job->queued_ns);
	ret = job->posted || job->buffer.alive(job->buffer.context) ?
		tbv_dmabuf_tx(job) : -ECANCELED;
	/* The rings are free for the next submission to post directly. */
	atomic_dec(&dmabuf_engine.tx_jobs);
	perf_end = tbv_perf_clock();
	tbv_perf_interval(&job->perf, "tx_queue", job->perf_queued_ns, perf_start, 0, 0);
	tbv_perf_interval(&job->perf, "tx_job", perf_start, perf_end, ret, 0);
	tbv_dmabuf_record(TBV_DMABUF_JOB, start);
	finish = tbv_dmabuf_clock();
	perf_finish = tbv_perf_clock();
	job->buffer.finish(job->buffer.context, ret);
	tbv_perf_span(&job->perf, "tx_finish", perf_finish, ret, 0);
	tbv_perf_span(&job->perf, "tx_wr", job->perf_queued_ns, ret, 0);
	tbv_dmabuf_record(TBV_DMABUF_TX_FINISH, finish);
	tbv_rail_put(job->route);
	kfree(job);
}

int tbv_dmabuf_submit(struct tbv_state *state, struct tbv_rail *rail,
		       const struct tbv_native_data_header *hdr,
		       const struct tbv_dmabuf_buffer *buffer, u8 retries,
		       u8 rnr_retries, unsigned long timeout)
{
	struct tbv_dmabuf_job *job;

	if (!dmabuf_engine.tx_wq || !rail || !tbv_dmabuf_rail_ready(rail))
		return -ENOTCONN;
	job = kzalloc(sizeof(*job), GFP_KERNEL);
	if (!job)
		return -ENOMEM;
	INIT_WORK(&job->work, tbv_dmabuf_tx_work);
	job->state = state;
	job->route = rail;
	job->hdr = *hdr;
	job->buffer = *buffer;
	job->retries = retries;
	job->rnr_retries = rnr_retries;
	job->timeout = timeout ?: msecs_to_jiffies(5000);
	job->queued_ns = tbv_dmabuf_clock();
	job->perf_queued_ns = tbv_perf_clock();
	if (job->perf_queued_ns) {
		job->perf.src_qp = hdr->src_qp;
		job->perf.wr_id = buffer->wr_id;
		job->perf.dst_qp = hdr->dest_qp;
		job->perf.psn = hdr->psn;
		job->perf.bytes = hdr->length;
		job->perf.tx = true;
		job->perf.opcode = hdr->opcode;
	}
	refcount_inc(&rail->refcnt);
	/* Queueing under the lock keeps a later WR behind one posted here. */
	mutex_lock(&dmabuf_engine.submit_lock);
	if (atomic_inc_return(&dmabuf_engine.tx_jobs) == 1 && dmabuf_direct_post)
		tbv_dmabuf_tx_direct(job);
	queue_work(dmabuf_engine.tx_wq, &job->work);
	mutex_unlock(&dmabuf_engine.submit_lock);
	return 0;
}

void tbv_dmabuf_disconnect(struct tbv_rail *rail)
{
	struct tbv_dmabuf_rx *rx = &dmabuf_engine.rx;
	u32 i;

	if (!dmabuf_verbs)
		return;
	mutex_lock(&dmabuf_engine.rx_lock);
	if (tbv_dmabuf_rx_active(rx) || rx->state == TBV_DMABUF_RX_SPECULATIVE) {
		for (i = 0; i < rx->batch.count; i++)
			if (rx->batch.lanes[i].rail == rail) {
				if (rx->state == TBV_DMABUF_RX_SPECULATIVE)
					tbv_dmabuf_spec_drop(true);
				else
					tbv_dmabuf_rx_finish(-ENOTCONN);
				break;
			}
	}
	if (dmabuf_engine.want_ctrl && rail->peer->dmabuf_control == dmabuf_engine.want_ctrl)
		tbv_dmabuf_want_clear();
	mutex_unlock(&dmabuf_engine.rx_lock);
	tbv_dmabuf_cache_purge(rail);
}

#ifdef TBV_BULK_NHI_ABI
static bool tbv_bulk_qualified(void)
{
	switch (LINUX_VERSION_CODE) {
	case KERNEL_VERSION(7, 2, 6):
	case KERNEL_VERSION(7, 2, 11):
	case KERNEL_VERSION(7, 3, 3):
		return true;
	}
	return false;
}
#endif

int tbv_dmabuf_start(struct tbv_state *state)
{
	if (!dmabuf_verbs)
		return 0;
	if (!dmabuf_post_burst || dmabuf_post_burst > TBV_DMABUF_BATCH_FRAMES)
		return -EINVAL;
#ifndef TBV_BULK_NHI_ABI
	if (dmabuf_batch_doorbell) {
		pr_err("dmabuf_batch_doorbell=1 needs the AMD NHI tb_ring layout on x86_64, which this kernel build does not have\n");
		return -EOPNOTSUPP;
	}
#else
	if (dmabuf_batch_doorbell && !tbv_bulk_qualified())
		pr_warn_once("dmabuf_batch_doorbell on unqualified kernel %s (qualified: 7.2.6, 7.2.11, 7.3.3)\n",
			     init_utsname()->release);
#endif
	if (dmabuf_poll_us && (dmabuf_poll_us < 10 || dmabuf_poll_us > 10000))
		return -EINVAL;
	/* Descriptor draining depends on Thunderbolt's ordinary ring workers;
	 * neither this queue nor the verbs error queue is a reclaim worker. */
	dmabuf_engine.tx_wq = alloc_ordered_workqueue("tbv_dmabuf_tx", 0);
	if (!dmabuf_engine.tx_wq)
		return -ENOMEM;
	dmabuf_engine.post_wq = alloc_workqueue("tbv_dmabuf_post", WQ_UNBOUND | WQ_HIGHPRI,
					TBV_DMABUF_MAX_RAILS);
	if (!dmabuf_engine.post_wq) {
		destroy_workqueue(dmabuf_engine.tx_wq);
		dmabuf_engine.tx_wq = NULL;
		return -ENOMEM;
	}
	atomic_set(&dmabuf_engine.progress_batches, 0);
	dmabuf_engine.progress_running = false;
	WRITE_ONCE(dmabuf_engine.progress_stopping, false);
	INIT_WORK(&dmabuf_engine.progress_work, tbv_dmabuf_progress_work);
	hrtimer_setup(&dmabuf_engine.progress_timer, tbv_dmabuf_progress_timer,
		      CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	if (dmabuf_poll_us) {
		dmabuf_engine.progress_wq = alloc_workqueue("tbv_dmabuf_progress",
			WQ_UNBOUND | WQ_HIGHPRI, 1);
		if (!dmabuf_engine.progress_wq) {
			destroy_workqueue(dmabuf_engine.post_wq);
			destroy_workqueue(dmabuf_engine.tx_wq);
			dmabuf_engine.post_wq = NULL;
			dmabuf_engine.tx_wq = NULL;
			return -ENOMEM;
		}
	}
	memset(&dmabuf_engine.rx, 0, sizeof(dmabuf_engine.rx));
	memset(dmabuf_engine.credits, 0, sizeof(dmabuf_engine.credits));
	dmabuf_engine.state = state;
	INIT_DELAYED_WORK(&dmabuf_engine.rx_watchdog, tbv_dmabuf_rx_watch);
	INIT_WORK(&dmabuf_engine.rx_complete_work, tbv_dmabuf_rx_completed);
	INIT_WORK(&dmabuf_engine.credit_work, tbv_dmabuf_credit_work);
	INIT_WORK(&dmabuf_engine.settle_work, tbv_dmabuf_settle_work);
	if (dmabuf_credits && !tbv_dmabuf_credits_enabled())
		pr_warn("dmabuf_credits needs dmabuf_push_done=1 and dmabuf_control_ring=0; credits stay off\n");
	return 0;
}

void tbv_dmabuf_stop(void)
{
	if (!dmabuf_engine.tx_wq)
		return;
	tbv_dmabuf_progress_stop();
	cancel_delayed_work_sync(&dmabuf_engine.rx_watchdog);
	cancel_work_sync(&dmabuf_engine.credit_work);
	mutex_lock(&dmabuf_engine.rx_lock);
	tbv_dmabuf_want_clear();
	if (dmabuf_engine.rx.state == TBV_DMABUF_RX_SPECULATIVE)
		tbv_dmabuf_spec_drop(true);
	if (tbv_dmabuf_rx_active(&dmabuf_engine.rx))
		tbv_dmabuf_rx_finish(-ECANCELED);
	mutex_unlock(&dmabuf_engine.rx_lock);
	cancel_work_sync(&dmabuf_engine.rx_complete_work);
	cancel_work_sync(&dmabuf_engine.credit_work);
	destroy_workqueue(dmabuf_engine.tx_wq);
	dmabuf_engine.tx_wq = NULL;
	destroy_workqueue(dmabuf_engine.post_wq);
	dmabuf_engine.post_wq = NULL;
	if (dmabuf_engine.progress_wq) {
		destroy_workqueue(dmabuf_engine.progress_wq);
		dmabuf_engine.progress_wq = NULL;
	}
}

/* The DMA-BUF attachment API used for imported GPU memory. */
MODULE_IMPORT_NS("DMA_BUF");
