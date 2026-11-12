// SPDX-License-Identifier: GPL-2.0
/*
 * Path TX reservations and the block build work, with the production
 * functions: a full path queue defers a block instead of dropping its
 * frames, an unreserved frame never takes another sender's reservation, and
 * a failed multi-path reservation gives back only what it reserved.
 */
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
typedef uint32_t u32; typedef uint64_t u64;
typedef long long atomic64_t;
#define atomic64_inc(p) (++*(p))
#define READ_ONCE(x) (x)
#define min_t(t, a, b) ((t)(a) < (t)(b) ? (t)(a) : (t)(b))
#define BIT(n) (1u << (n))
#define spin_lock_irqsave(l, f) ((void)(l), (void)(f))
#define spin_unlock_irqrestore(l, f) ((void)(l), (void)(f))
#define TBV_PATH_SEND_CONTROL BIT(0)
#define TBV_PATH_SEND_DEFER BIT(1)
#define TBV_PATH_SEND_RESERVED BIT(2)
#define TBV_PATH_TUNNEL_ENABLED 2
#define IB_QPS_ERR 6
#define TBV_SEND_BUILD_CHUNK 64u
#define container_of(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))
struct list_head { struct list_head *next, *prev; };
#define LIST_HEAD_INIT(n) { &(n), &(n) }
static void list_add_tail(struct list_head *n, struct list_head *h)
{ n->next = h; n->prev = h->prev; h->prev->next = n; h->prev = n; }
static void list_splice_tail_init(struct list_head *l, struct list_head *h)
{
	if (l->next == l)
		return;
	l->next->prev = h->prev; h->prev->next = l->next;
	l->prev->next = h; h->prev = l->prev;
	l->next = l->prev = l;
}
#define list_for_each_entry(p, h, m) \
	for (p = container_of((h)->next, __typeof__(*p), m); &p->m != (h); \
	     p = container_of(p->m.next, __typeof__(*p), m))

struct tbv_tx_packet { struct list_head node; u32 start_credit_group_frames; bool queued; };
struct tbv_path {
	int tx_lock, state;
	u32 tx_data_queued, tx_data_reserved, tx_data_queue_limit;
	struct list_head tx_data_queue;
	atomic64_t data_tx_enqueued;
};
struct tbv_state { atomic64_t data_wr_block_deferred; };
struct tbv_qp { struct tbv_state *owner; bool closing; int state; };
struct tbv_send_ctx { struct tbv_qp *tqp; bool completed; u32 retired, next_frag; bool out_of_order; };
struct tbv_send_build_job { struct tbv_send_ctx *send; struct tbv_path *path; u32 first_frag, nfrags; };

static void tbv_path_schedule_tx(struct tbv_path *path) { (void)path; }
static void tbv_send_retire_frames(struct tbv_send_ctx *s, u32 n, int status)
{ (void)status; s->retired += n; }

#include "path-helpers.c"

/* Model of the production chunk builder: reserve, then enqueue the chunk. */
static int tbv_native_send_build_chunk(struct tbv_send_ctx *ctx,
				       struct tbv_path *path, u32 first_frag,
				       u32 nfrags)
{
	struct tbv_tx_packet *packets = calloc(nfrags, sizeof(*packets));
	struct list_head list = LIST_HEAD_INIT(list);
	int ret = tbv_path_reserve_data(path, nfrags);
	u32 i;

	if (ret == -ENOMEM) {
		free(packets);
		return -EAGAIN;
	}
	if (ret) {
		free(packets);
		return ret;
	}
	if (first_frag != ctx->next_frag)
		ctx->out_of_order = true;
	ctx->next_frag = first_frag + nfrags;
	for (i = 0; i < nfrags; i++)
		list_add_tail(&packets[i].node, &list);
	ret = tbv_path_enqueue_reserved_data_list(path, &list, nfrags, true);
	/* The harness only counts; the queue drains by resetting it below. */
	path->tx_data_queue.next = path->tx_data_queue.prev = &path->tx_data_queue;
	free(packets);
	return ret;
}

#include "ibdev-helpers.c"

static struct tbv_path new_path(u32 limit)
{
	return (struct tbv_path){ .state = TBV_PATH_TUNNEL_ENABLED,
				  .tx_data_queue_limit = limit };
}

static int expect(bool ok, const char *what)
{
	if (!ok)
		fprintf(stderr, "FAIL %s\n", what);
	return !ok;
}

int main(void)
{
	struct tbv_tx_packet single = {0};
	struct tbv_state owner = {0};
	struct tbv_qp qp = { .owner = &owner };
	struct tbv_send_ctx ctx = { .tqp = &qp };
	struct tbv_path p = new_path(10), a = new_path(100), b = new_path(5),
			c = new_path(100);
	struct tbv_path *paths[] = { &a, &b, &c };
	u32 reservations[] = { 10, 10, 10 };
	struct tbv_send_build_job job;
	struct list_head list = LIST_HEAD_INIT(list);
	struct tbv_tx_packet four[4] = {0};
	int rounds = 0, i;

	/* An RX credit or ACK must not use a block's reservation. */
	p.tx_data_queue = (struct list_head)LIST_HEAD_INIT(p.tx_data_queue);
	if (expect(!tbv_path_reserve_data(&p, 4), "reserve"))
		return 1;
	if (expect(!tbv_path_enqueue_data(&p, &single, TBV_PATH_SEND_DEFER) &&
		   p.tx_data_reserved == 4 && p.tx_data_queued == 1,
		   "unreserved frame took a reservation"))
		return 1;
	for (i = 0; i < 4; i++)
		list_add_tail(&four[i].node, &list);
	if (expect(!tbv_path_enqueue_reserved_data_list(&p, &list, 4, true) &&
		   p.tx_data_reserved == 0 && p.tx_data_queued == 5,
		   "reserved block enqueue failed after an unreserved frame"))
		return 1;
	p.tx_data_queue = (struct list_head)LIST_HEAD_INIT(p.tx_data_queue);
	single = (struct tbv_tx_packet){0};
	if (expect(!tbv_path_reserve_data(&p, 1) &&
		   !tbv_path_enqueue_data(&p, &single, TBV_PATH_SEND_DEFER |
					  TBV_PATH_SEND_RESERVED) &&
		   p.tx_data_reserved == 0, "reserved single frame"))
		return 1;

	/* A failed multi-path reservation gives back only what it reserved. */
	b.tx_data_queued = 5;
	if (expect(tbv_reserve_path_frames(paths, reservations, 3) == -ENOMEM &&
		   reservations[0] == 10 && !reservations[1] && !reservations[2] &&
		   a.tx_data_reserved == 10 && !c.tx_data_reserved,
		   "failed reservation left planned counts"))
		return 1;
	c.tx_data_reserved = 7;	/* another sender's reservation */
	tbv_release_path_reservations(paths, reservations, 3);
	if (expect(!a.tx_data_reserved && c.tx_data_reserved == 7,
		   "release took another sender's reservation"))
		return 1;

	/* A full queue defers the block; it resumes in order, nothing dropped. */
	a = new_path(100);
	a.tx_data_queue = (struct list_head)LIST_HEAD_INIT(a.tx_data_queue);
	job = (struct tbv_send_build_job){ &ctx, &a, 0, 300 };
	while (tbv_native_send_run_build_job(&job)) {
		a.tx_data_queued = 0;	/* the ring sent the queue */
		if (expect(++rounds < 10, "deferred block never finished"))
			return 1;
	}
	if (expect(!ctx.retired && ctx.next_frag == 300 && !ctx.out_of_order &&
		   owner.data_wr_block_deferred == rounds && rounds >= 2,
		   "deferred block dropped or reordered frames"))
		return 1;

	/* A send that needs no more frames, or a path that is down, retires. */
	ctx = (struct tbv_send_ctx){ .tqp = &qp, .completed = true };
	job = (struct tbv_send_build_job){ &ctx, &a, 0, 300 };
	if (expect(!tbv_native_send_run_build_job(&job) && ctx.retired == 300,
		   "completed send kept its frames"))
		return 1;
	ctx = (struct tbv_send_ctx){ .tqp = &qp };
	a.tx_data_queued = 0;
	a.state = 0;
	job = (struct tbv_send_build_job){ &ctx, &a, 0, 300 };
	if (expect(!tbv_native_send_run_build_job(&job) && ctx.retired == 300,
		   "down path did not retire the block"))
		return 1;

	puts("Native block build: full queue defers in order, reservations stay with their owner");
	return 0;
}
