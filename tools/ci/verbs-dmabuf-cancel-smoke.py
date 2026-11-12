#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Destroying a QP with a dmabuf WR in flight: tbv_dmabuf_cancel() stops the
rails only when the WR's posted frames do not drain. An unposted or drained
batch leaves them usable (stopped rails stay quarantined until reload)."""
from smoke_support import ROOT, functions, run_c

dmabuf = (ROOT / 'kernel/verbs_dmabuf.c').read_text()
shim = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
typedef unsigned int u32;
typedef int atomic_t;
typedef long long atomic64_t;
#define atomic_read(p) (*(p))
#define atomic64_inc(p) (++*(p))
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x, v) ((x) = (v))
#define msecs_to_jiffies(ms) (ms)
#define time_before(a, b) ((long)((a) - (b)) < 0)
#define mutex_lock(m) (assert(!*(m)), *(m) = 1)
#define mutex_unlock(m) (assert(*(m)), *(m) = 0)
static unsigned long jiffies;
static bool dmabuf_verbs = true;
static atomic64_t dmabuf_cancel_unposted, dmabuf_cancel_drained, dmabuf_cancel_stopped;
struct tbv_rail { int dmabuf_quarantine_reason; bool stopped; };
struct tbv_dmabuf_lane { struct tbv_rail *rail; };
struct tbv_dmabuf_batch {
	struct tbv_dmabuf_lane lanes[2];
	atomic_t pending, error;
	int wait;
	u32 count;
	bool submitted, tx, canceled;
};
static struct { int tx_lock; void *active_context; struct tbv_dmabuf_batch *active_batch; } dmabuf_engine;
/* Each wait either completes the DMA after `drain_after` waits or times out. */
static int waits, kicks, drain_after = -1, fail_after = -1;
static long simulated_wait(struct tbv_dmabuf_batch *batch, long timeout)
{
	waits++;
	jiffies += timeout;
	if (waits == drain_after)
		batch->pending = 0;
	if (waits == fail_after)
		batch->error = -5;
	return batch->pending && !batch->error ? 0 : 1;
}
#define wait_event_timeout(q, cond, t) \
	((cond) ? (t) : simulated_wait(container_of_wait(&(q)), (t)))
#define container_of_wait(q) ((struct tbv_dmabuf_batch *)((char *)(q) - \
	__builtin_offsetof(struct tbv_dmabuf_batch, wait)))
static void tbv_dmabuf_stop_rail(struct tbv_rail *rail)
{
	assert(dmabuf_engine.tx_lock);
	rail->stopped = true;
}
static void tbv_dmabuf_batch_kick(struct tbv_dmabuf_batch *batch, bool tx) { kicks++; }
"""
cases = r"""
static struct tbv_rail r0, r1;
static struct tbv_dmabuf_batch batch;
static int ctx, other;
static void reset(bool submitted)
{
	r0 = r1 = (struct tbv_rail){0};
	batch = (struct tbv_dmabuf_batch){ .lanes = { { &r0 }, { &r1 } }, .count = 2,
		.pending = 64, .submitted = submitted, .tx = true };
	dmabuf_engine.active_context = &ctx;
	dmabuf_engine.active_batch = &batch;
	dmabuf_cancel_unposted = dmabuf_cancel_drained = dmabuf_cancel_stopped = 0;
	jiffies = 1000; waits = kicks = 0; drain_after = fail_after = -1;
}
static bool stopped(void) { return r0.stopped || r1.stopped; }
int main(void)
{
	reset(true); tbv_dmabuf_cancel(&other);
	assert(!stopped() && !batch.canceled);	/* another WR's batch */
	reset(false); tbv_dmabuf_cancel(&ctx);
	assert(!stopped() && batch.canceled && dmabuf_cancel_unposted == 1);
	reset(true); batch.pending = 0; tbv_dmabuf_cancel(&ctx);
	assert(!stopped() && !waits && dmabuf_cancel_drained == 1);
	reset(true); drain_after = 3; tbv_dmabuf_cancel(&ctx);
	assert(!stopped() && waits == 3 && kicks == 2 && dmabuf_cancel_drained == 1);
	reset(true); fail_after = 2; tbv_dmabuf_cancel(&ctx);
	assert(r0.stopped && r1.stopped && r0.dmabuf_quarantine_reason == 2 &&
	       dmabuf_cancel_stopped == 1);
	reset(true); tbv_dmabuf_cancel(&ctx);	/* never drains: bounded */
	assert(r0.stopped && r1.stopped && waits == 20 && dmabuf_cancel_stopped == 1);
	reset(true); drain_after = 1; dmabuf_verbs = false; tbv_dmabuf_cancel(&ctx);
	assert(!waits && !batch.canceled);
	puts("dmabuf cancel: unposted or drained batches keep their rails; stuck ones stop");
	return 0;
}
"""
run_c('verbs-dmabuf-cancel', shim + "#define TBV_DMABUF_CANCEL_DRAIN_MS 200\n" +
      functions(dmabuf, ['tbv_dmabuf_batch_drain', 'tbv_dmabuf_cancel']) + cases)
