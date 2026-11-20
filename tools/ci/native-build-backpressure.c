// SPDX-License-Identifier: GPL-2.0
/*
 * Path TX staging-frame claims and block build work, with the production
 * functions: claims respect the control reserve, released frames return to
 * tx_free, and a full path defers a block in order without dropping frames.
 */
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
typedef uint8_t u8; typedef uint32_t u32; typedef uint64_t u64;
typedef long long atomic64_t;
#define atomic64_inc(p) (++*(p))
#define atomic64_add(v, p) (*(p) += (v))
#define READ_ONCE(x) (x)
#define min_t(t, a, b) ((t)(a) < (t)(b) ? (t)(a) : (t)(b))
#define clamp_t(t, v, lo, hi) \
	((t)(v) < (t)(lo) ? (t)(lo) : ((t)(v) > (t)(hi) ? (t)(hi) : (t)(v)))
#define BIT(n) (1u << (n))
#define spin_lock_irqsave(l, f) ((void)(l), (void)(f))
#define spin_unlock_irqrestore(l, f) ((void)(l), (void)(f))
#define TBV_PATH_SEND_CONTROL BIT(0)
#define TBV_PATH_SEND_DEFER BIT(1)
#define TBV_DATA_CREDIT_CONTROL_RESERVE 256u
#define TBV_DATA_PDF_FRAME_START 1
#define TBV_DATA_PDF_FRAME_END 3
#define TBV_PATH_TUNNEL_ENABLED 2
#define IB_QPS_ERR 6
#define TBV_SEND_BUILD_CHUNK 64u
#define container_of(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))
struct list_head { struct list_head *next, *prev; };
#define LIST_HEAD_INIT(n) { &(n), &(n) }
#define INIT_LIST_HEAD(p) do { (p)->next = (p); (p)->prev = (p); } while (0)
static bool list_empty(const struct list_head *h) { return h->next == h; }
static void list_add_tail(struct list_head *n, struct list_head *h)
{ n->next = h; n->prev = h->prev; h->prev->next = n; h->prev = n; }
static void list_del_init(struct list_head *e)
{ e->prev->next = e->next; e->next->prev = e->prev; INIT_LIST_HEAD(e); }
static void list_splice_tail_init(struct list_head *l, struct list_head *h)
{
	if (l->next == l)
		return;
	l->next->prev = h->prev; h->prev->next = l->next;
	l->prev->next = h; h->prev = l->prev;
	l->next = l->prev = l;
}
#define list_first_entry(h, t, m) container_of((h)->next, t, m)
#define list_for_each_entry(p, h, m) \
	for (p = container_of((h)->next, __typeof__(*p), m); &p->m != (h); \
	     p = container_of(p->m.next, __typeof__(*p), m))
#define list_for_each_entry_safe(p, n, h, m) \
	for (p = container_of((h)->next, __typeof__(*p), m), \
	     n = container_of(p->m.next, __typeof__(*p), m); \
	     &p->m != (h); \
	     p = n, n = container_of(n->m.next, __typeof__(*n), m))

struct tbv_tx_packet;
typedef void (*tbv_path_tx_done_fn)(void *ctx, int status);
struct tbv_data_frame {
	struct list_head node;
	struct tbv_tx_packet *packet;
	tbv_path_tx_done_fn done;
	void *done_ctx;
	void *owner_ctx;
	u32 len;
	u32 start_credit_group_frames;
	u8 sof;
	u8 eof;
	bool queued;
	bool inflight;
};
struct tbv_path {
	int tx_lock, state;
	u32 tx_frame_count, tx_free_count, tx_claimed, tx_data_queued;
	struct list_head tx_free, tx_data_queue;
	atomic64_t data_tx_enqueued;
};
struct tbv_state { atomic64_t data_wr_block_deferred; };
struct tbv_qp { struct tbv_state *owner; bool closing; int state; };
struct tbv_send_ctx { struct tbv_qp *tqp; bool completed; u32 retired, next_frag; bool out_of_order; };
struct tbv_send_build_job {
	struct tbv_send_ctx *send;
	struct tbv_path *path;
	u32 first_frag, frag_stride, nfrags;
	bool block_striping;
};

static void tbv_path_schedule_tx(struct tbv_path *path) { (void)path; }
static void tbv_send_retire_frames(struct tbv_send_ctx *s, u32 n, int status)
{ (void)status; s->retired += n; }

#include "path-helpers.c"

/* Model of the production chunk builder: claim staging frames, then enqueue. */
static int tbv_native_send_build_chunk(struct tbv_send_ctx *ctx,
				       struct tbv_path *path, u32 first_frag,
				       u32 frag_stride, u32 nfrags,
				       bool block_striping)
{
	struct list_head list = LIST_HEAD_INIT(list);
	int ret = tbv_path_claim_tx_frames(path, nfrags, &list);

	(void)frag_stride;
	(void)block_striping;
	if (ret == -ENOMEM)
		return -EAGAIN;
	if (ret)
		return ret;
	if (first_frag != ctx->next_frag)
		ctx->out_of_order = true;
	ctx->next_frag = first_frag + nfrags;
	return tbv_path_enqueue_tx_frames(path, &list, nfrags,
					  TBV_PATH_SEND_DEFER);
}

#include "ibdev-helpers.c"

static void init_path(struct tbv_path *p, struct tbv_data_frame *pool, u32 n)
{
	u32 i;

	*p = (struct tbv_path){ .state = TBV_PATH_TUNNEL_ENABLED,
				.tx_frame_count = n,
				.tx_free_count = n };
	INIT_LIST_HEAD(&p->tx_free);
	INIT_LIST_HEAD(&p->tx_data_queue);
	for (i = 0; i < n; i++) {
		pool[i] = (struct tbv_data_frame){0};
		INIT_LIST_HEAD(&pool[i].node);
		list_add_tail(&pool[i].node, &p->tx_free);
	}
}

static void drain_path(struct tbv_path *p)
{
	tbv_path_return_tx_frames_locked(p, &p->tx_data_queue, false);
	p->tx_data_queued = 0;
}

static int expect(bool ok, const char *what)
{
	if (!ok)
		fprintf(stderr, "FAIL %s\n", what);
	return !ok;
}

int main(void)
{
	struct tbv_state owner = {0};
	struct tbv_qp qp = { .owner = &owner };
	struct tbv_send_ctx ctx = { .tqp = &qp };
	struct tbv_data_frame pool[160];
	struct tbv_path p;
	struct tbv_send_build_job job;
	struct list_head list = LIST_HEAD_INIT(list);
	int rounds = 0;

	/* Control reserve: 16 frames => reserve = 4, max claimable = 12. */
	init_path(&p, pool, 16);
	if (expect(tbv_path_claim_tx_frames(&p, 13, &list) == -ENOMEM &&
		   p.tx_free_count == 16 && p.tx_claimed == 0,
		   "claim violated control reserve"))
		return 1;
	if (expect(!tbv_path_claim_tx_frames(&p, 12, &list) &&
		   p.tx_free_count == 4 && p.tx_claimed == 12,
		   "claim up to control reserve failed"))
		return 1;
	tbv_path_release_tx_frames(&p, &list);
	if (expect(p.tx_free_count == 16 && p.tx_claimed == 0,
		   "release did not restore tx_free_count"))
		return 1;

	/*
	 * A full staging ring defers the block; it resumes in order, nothing
	 * dropped. 160 frames => reserve 40 => 120 claimable (1 chunk of 64).
	 */
	init_path(&p, pool, 160);
	job = (struct tbv_send_build_job){ &ctx, &p, 0, 1, 300, true };
	while (tbv_native_send_run_build_job(&job)) {
		drain_path(&p);
		if (expect(++rounds < 10, "deferred block never finished"))
			return 1;
	}
	if (expect(!ctx.retired && ctx.next_frag == 300 && !ctx.out_of_order &&
		   owner.data_wr_block_deferred == rounds && rounds >= 2,
		   "deferred block dropped or reordered frames"))
		return 1;

	/* A send that needs no more frames, or a path that is down, retires. */
	drain_path(&p);
	ctx = (struct tbv_send_ctx){ .tqp = &qp, .completed = true };
	job = (struct tbv_send_build_job){ &ctx, &p, 0, 1, 300, true };
	if (expect(!tbv_native_send_run_build_job(&job) && ctx.retired == 300,
		   "completed send kept its frames"))
		return 1;
	ctx = (struct tbv_send_ctx){ .tqp = &qp };
	p.state = 0;
	job = (struct tbv_send_build_job){ &ctx, &p, 0, 1, 300, true };
	if (expect(!tbv_native_send_run_build_job(&job) && ctx.retired == 300,
		   "down path did not retire the block"))
		return 1;

	puts("Native block build: staging claims respect control reserve, full ring defers in order");
	return 0;
}
