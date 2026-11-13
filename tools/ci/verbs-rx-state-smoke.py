#!/usr/bin/env python3
"""Exercise actual RX transitions, including pre-armed SEND credits, with
simulated DMA, ownership callbacks and control notifications."""
from smoke_support import ROOT, declaration, functions, run_c

source = (ROOT / "kernel/verbs_dmabuf.c").read_text()
state = declaration(source, r"^enum tbv_dmabuf_rx_state \{[\s\S]*?^\};")
counters = declaration(source, r"^enum tbv_dmabuf_credit_counter \{[\s\S]*?^\};")
body = functions(source, (
    "tbv_dmabuf_rx_active", "tbv_dmabuf_same", "tbv_dmabuf_rx_notify",
    "tbv_dmabuf_rx_finish", "tbv_dmabuf_want_clear", "tbv_dmabuf_cancel_recv",
    "tbv_dmabuf_rx_reap_impl", "tbv_dmabuf_credit_match", "tbv_dmabuf_token_fresh",
    "tbv_dmabuf_spec_adopt", "tbv_dmabuf_spec_drop", "tbv_dmabuf_spec_cancel",
    "tbv_dmabuf_spec_poll", "tbv_dmabuf_credit_arm", "tbv_dmabuf_credit_next",
    "tbv_dmabuf_credit_work", "tbv_dmabuf_recv_posted", "tbv_dmabuf_credit_commit",
    "tbv_dmabuf_spec_prepare", "tbv_dmabuf_rx_cached", "tbv_dmabuf_rx_prepare",
    "tbv_dmabuf_control_request", "tbv_dmabuf_disconnect",
    "tbv_dmabuf_rx_cancel_due", "tbv_dmabuf_rx_watch",
))
shim = r"""
#include <assert.h>
#include <stdio.h>
#include "proto/verbs_dmabuf.h"
#include "tools/ci/perf-noop.h"
typedef uint32_t u32;
typedef uint64_t u64;
struct work_struct { int unused; };
struct list_head { struct list_head *next; };
#define list_for_each_entry(pos, head, member) \
    for (struct list_head *n = (head)->next; n != (head) && \
         ((pos) = (void *)n, 1); n = n->next)
#define mutex_lock(p) ((void)(p))
#define mutex_unlock(p) ((void)(p))
#define atomic_read(p) (*(p))
#define msecs_to_jiffies(ms) (ms)
#define dma_rmb() ((void)0)
#define min_t(type, a, b) ((type)(a) < (type)(b) ? (type)(a) : (type)(b))
#define mod_delayed_work(wq, work, delay) ((void)(delay))
#define READ_ONCE(x) (x)
#define time_after_eq(a, b) ((long)((a) - (b)) >= 0)
#define wake_up_all(q) ((void)(q))
/* Waits observe the state after wait_hook, which stages what happens meanwhile. */
static void (*wait_hook)(void);
static void run_wait_hook(void) { void (*hook)(void) = wait_hook; wait_hook = NULL; if (hook) hook(); }
static long mock_wait_timeout(bool condition) { return condition ? 1 : 0; }
#define wait_event_timeout(q, cond, t) ((void)(q), (void)(t), run_wait_hook(), mock_wait_timeout(cond))
typedef long long atomic64_t;
#define atomic64_inc(p) (++*(p))
#define TBV_DMABUF_CANCEL_DRAIN_MS 200
static atomic64_t dmabuf_cancel_rx_drained, dmabuf_cancel_rx_stopped;
#define queue_work(wq, work) mock_queue_work()
#define pr_warn_ratelimited(...) ((void)0)
static unsigned long jiffies;
static bool dmabuf_verbs = true;
static bool dmabuf_credits = true;
static unsigned int dmabuf_credit_max = 1u << 20;
static unsigned int queued;
static bool mock_queue_work(void) { queued++; return true; }
static int begin_result, alloc_result, post_result, copy_result, finish_result;
static unsigned int begins, finishes, copies, drops, ref_gets, ref_puts, notifications;
static int notified;
struct tbv_control { int refs; };
struct tbv_peer;
struct tbv_rail { int refs; struct tbv_peer *peer; };
struct tbv_state { int lock; struct list_head peers; };
struct tbv_peer {
    struct list_head node;
    struct tbv_state *state;
    struct tbv_control *dmabuf_control;
    bool remote_host_known;
    unsigned char remote_host_id[16];
    u64 dmabuf_rx_token;
};
struct tbv_dmabuf_buffer {
    void *mr, *host_mr, *owner, *context;
    int (*finish)(void *, int);
    int (*copy)(void *, u32, void *, u32, bool);
    bool (*alive)(void *);
};
struct tbv_dmabuf_lane {
    struct tbv_rail *rail;
    u32 first, count, staging_bytes;
    void *staging;
};
struct tbv_dmabuf_batch {
    struct tbv_dmabuf_lane lanes[TBV_DMABUF_MAX_RAILS];
    u32 count;
    int pending, error, wait;
};
"""
rx = declaration(source, r"^struct tbv_dmabuf_rx \{[\s\S]*?^\};")
mocks = r"""
static struct {
    struct tbv_dmabuf_rx rx;
    int rx_lock, rx_watchdog, rx_wait, credit_work, post_wq_storage;
    int *post_wq;
    struct tbv_state *state;
    void *want_owner;
    struct tbv_dmabuf_wire want;
    struct tbv_control *want_ctrl;
    u32 credit_seq;
} dmabuf_engine;
static struct tbv_rail rail;
static bool lane_available = true;
static unsigned int counts[CREDIT_COUNTERS], notifies, stops, peeks, claims;
static int peek_result, claim_result, notify_result;
static bool credits_on = true, alive = true;
static struct tbv_dmabuf_wire last_notify;
static void tbv_dmabuf_credit_count(enum tbv_dmabuf_credit_counter c) { counts[c]++; }
static bool tbv_control_credits(const struct tbv_control *ctrl) { return ctrl && credits_on; }
static int tbv_control_notify(struct tbv_control *ctrl, const struct tbv_dmabuf_wire *m) {
    notifies++; last_notify = *m; return notify_result;
}
static void tbv_dmabuf_stop_rail(struct tbv_rail *r) { stops++; }
/* Allocation/lease ownership is exercised with real cache helpers in the
 * Framework regression suite; this shim isolates receive transitions. */
static void tbv_dmabuf_cache_purge(struct tbv_rail *r) { (void)r; }
static bool buffer_alive(void *ctx) { return alive; }
static int tbv_ibdev_dmabuf_peek(struct tbv_state *s, struct tbv_rail *r,
                                struct tbv_native_data_header *h,
                                struct tbv_dmabuf_buffer *buffer);
static int tbv_ibdev_dmabuf_claim(struct tbv_dmabuf_buffer *buffer,
                                 const struct tbv_native_data_header *h) {
    claims++; return claim_result;
}
static void tbv_dmabuf_credit_next(struct tbv_control *ctrl);
static void tbv_dmabuf_spec_cancel(void);
static void tbv_dmabuf_spec_drop(bool abort);
static bool tbv_dmabuf_spec_poll(void);
static bool tbv_dmabuf_rx_cached(const struct tbv_dmabuf_wire *m);
static int tbv_dmabuf_spec_prepare(struct tbv_state *state, struct tbv_dmabuf_wire *m);
static u64 tbv_dmabuf_clock(void) { return 0; }
enum { TBV_DMABUF_RX_BUILD, TBV_DMABUF_RX_POST };
static void tbv_dmabuf_record(int stage, u64 start) { (void)stage; (void)start; }
static void tbv_control_get(struct tbv_control *ctrl) { ref_gets++; ctrl->refs++; }
static void tbv_control_put(struct tbv_control *ctrl) { if (ctrl) { ref_puts++; ctrl->refs--; } }
static void tbv_control_done(struct tbv_control *ctrl, const struct tbv_dmabuf_wire *m, int status) {
    if (ctrl) { notifications++; notified = status; }
}
static int finish(void *ctx, int status) { finishes++; return status ?: finish_result; }
static int copy(void *ctx, u32 offset, void *data, u32 bytes, bool to_mr) {
    assert(to_mr); copies++; return copy_result;
}
static int tbv_ibdev_dmabuf_begin(struct tbv_state *s, struct tbv_rail *r,
                                const struct tbv_native_data_header *h,
                                struct tbv_dmabuf_buffer *buffer) {
    begins++;
    if (!begin_result)
        *buffer = (struct tbv_dmabuf_buffer){ .owner = &rail, .finish = finish,
            .copy = copy, .alive = buffer_alive };
    return begin_result;
}
static int gpu_mr;
/* Like ibdev: predicts the next PSN; an empty RQ still names its owner. */
static int tbv_ibdev_dmabuf_peek(struct tbv_state *s, struct tbv_rail *r,
                                struct tbv_native_data_header *h,
                                struct tbv_dmabuf_buffer *buffer) {
    peeks++;
    h->psn++;
    if (peek_result == -EAGAIN) buffer->owner = &rail;
    if (!peek_result)
        *buffer = (struct tbv_dmabuf_buffer){ .mr = &gpu_mr, .owner = &rail,
            .finish = finish, .copy = copy, .alive = buffer_alive };
    return peek_result;
}
static struct tbv_rail *tbv_dmabuf_find_lane(struct tbv_state *s,
                                           const struct tbv_dmabuf_wire *m, u32 i) {
    if (!lane_available) return NULL;
    rail.refs++;
    return &rail;
}
static void tbv_rail_put(struct tbv_rail *r) { r->refs--; }
static unsigned int stopping_drops;
static void tbv_dmabuf_batch_drop(struct tbv_dmabuf_batch *batch, bool stop) {
    drops++;
    if (stop && batch->count) stopping_drops++;
    for (u32 i = 0; i < batch->count; i++) tbv_rail_put(batch->lanes[i].rail);
    memset(batch, 0, sizeof(*batch));
}
static int tbv_dmabuf_batch_alloc(struct tbv_dmabuf_batch *batch,
                                 const struct tbv_dmabuf_wire *m,
                                 const struct tbv_dmabuf_buffer *b, bool tx) {
    batch->pending = !!m->bytes;
    batch->lanes[0].count = !!m->bytes;
    batch->lanes[0].staging_bytes = m->bytes;
    return alloc_result;
}
static int tbv_dmabuf_batch_post(struct tbv_dmabuf_batch *b, bool tx) { return post_result; }
static unsigned int kicks;
static void tbv_dmabuf_batch_kick(struct tbv_dmabuf_batch *b, bool tx) { kicks++; }
static int system_unbound_wq __attribute__((unused));
static int tbv_dmabuf_rx_reap(void);
"""
# The wrapper only records timings; tests exercise the actual reap implementation.
body = body.replace("static int tbv_dmabuf_rx_prepare", "static int tbv_dmabuf_rx_reap(void) { return tbv_dmabuf_rx_reap_impl(); }\n\nstatic int tbv_dmabuf_rx_prepare")
cases = r"""
static void complete_armed_batch(void) {
    dmabuf_engine.rx.batch.pending = 0;
    assert(tbv_dmabuf_rx_reap_impl() == TBV_DMABUF_DONE);
}
int main(void) {
    struct tbv_state state = {0};
    struct tbv_peer peer = { .state = &state, .remote_host_known = true };
    struct tbv_control ctrl = { .refs = 1 };
    struct tbv_dmabuf_wire m = { .op = TBV_DMABUF_PREPARE, .token = 1,
        .count = 1, .bytes = 4096, .hdr.length = 8192 };
    peer.node.next = &state.peers; state.peers.next = &peer.node;
    assert(dmabuf_engine.rx.state == TBV_DMABUF_RX_IDLE);
    assert(tbv_dmabuf_control_request(&peer, &ctrl, &m) == TBV_DMABUF_READY);
    assert(begins == 1 && dmabuf_engine.rx.state == TBV_DMABUF_RX_ARMED);
    assert(ctrl.refs == 2 && ref_gets == 1);
    assert(tbv_dmabuf_control_request(&peer, &ctrl, &m) == TBV_DMABUF_READY);
    assert(begins == 1 && ref_gets == 1);
    struct tbv_dmabuf_wire other = m; other.token++;
    assert(tbv_dmabuf_control_request(&peer, &ctrl, &other) == TBV_DMABUF_BUSY);
    dmabuf_engine.rx.batch.pending = 0;
    assert(tbv_dmabuf_rx_reap_impl() == TBV_DMABUF_DONE);
    assert(dmabuf_engine.rx.state == TBV_DMABUF_RX_OPEN && !finishes);
    assert(ctrl.refs == 1 && notifications == 1 && notified == TBV_DMABUF_DONE);
    assert(tbv_dmabuf_control_request(&peer, &ctrl, &m) == TBV_DMABUF_DONE);
    m.offset = 4096;
    assert(tbv_dmabuf_control_request(&peer, &ctrl, &m) == TBV_DMABUF_READY);
    dmabuf_engine.rx.batch.pending = 0;
    assert(tbv_dmabuf_rx_reap_impl() == TBV_DMABUF_DONE);
    assert(dmabuf_engine.rx.state == TBV_DMABUF_RX_CACHED && finishes == 1);
    assert(ctrl.refs == 1 && notifications == 2 && copies == 2 && !rail.refs);
    assert(tbv_dmabuf_control_request(&peer, &ctrl, &m) == TBV_DMABUF_DONE);
    assert(finishes == 1);
    m.offset = 0;
    assert(tbv_dmabuf_control_request(&peer, &ctrl, &m) == -ESTALE);
    // Empty barrier finishes its WR directly in PREPARE, with no DONE reference.
    m.token++; m.bytes = m.hdr.length = 0;
    assert(tbv_dmabuf_control_request(&peer, &ctrl, &m) == TBV_DMABUF_DONE);
    assert(finishes == 2 && notifications == 2 && ctrl.refs == 1);
    for (int error = 0; error < 5; error++) {
        m.token++; m.bytes = m.hdr.length = 4096;
        assert(tbv_dmabuf_control_request(&peer, &ctrl, &m) == TBV_DMABUF_READY);
        unsigned int before = finishes;
        if (error == 0) tbv_dmabuf_cancel_recv(&rail);
        else if (error == 1) tbv_dmabuf_disconnect(&rail);
        else if (error == 2) { dmabuf_engine.rx.batch.error = -EIO; assert(tbv_dmabuf_rx_reap_impl() == -EIO); }
        else if (error == 3) { copy_result = -EFAULT; dmabuf_engine.rx.batch.pending = 0; assert(tbv_dmabuf_rx_reap_impl() == -EFAULT); copy_result = 0; }
        else { finish_result = -ENOSPC; dmabuf_engine.rx.batch.pending = 0; assert(tbv_dmabuf_rx_reap_impl() == -ENOSPC); finish_result = 0; }
        assert(finishes == before + 1 && dmabuf_engine.rx.state == TBV_DMABUF_RX_CACHED);
        assert(ctrl.refs == 1 && !rail.refs && ref_gets == ref_puts);
        tbv_dmabuf_cancel_recv(&rail); assert(finishes == before + 1);
    }
    assert(dmabuf_cancel_rx_stopped == 1 && !dmabuf_cancel_rx_drained);
    // QP destroy with an armed batch: the sender was told READY and posts, so
    // the frames land during the wait and the batch completes normally.
    m.token++;
    assert(tbv_dmabuf_control_request(&peer, &ctrl, &m) == TBV_DMABUF_READY);
    {
        unsigned int before = finishes, stops_before = stopping_drops;
        wait_hook = complete_armed_batch;
        tbv_dmabuf_cancel_recv(&rail);
        assert(finishes == before + 1 && notified == TBV_DMABUF_DONE);
        assert(stopping_drops == stops_before && dmabuf_cancel_rx_drained == 1);
        assert(dmabuf_engine.rx.state == TBV_DMABUF_RX_CACHED && !rail.refs);
    }
    // The watchdog gives a dead WR's armed batch the same grace, then cancels.
    for (int lands = 1; lands >= 0; lands--) {
        m.token++;
        assert(tbv_dmabuf_control_request(&peer, &ctrl, &m) == TBV_DMABUF_READY);
        unsigned int before = finishes, stops_before = stopping_drops;
        alive = false;
        tbv_dmabuf_rx_watch(NULL);
        assert(finishes == before && dmabuf_engine.rx.state == TBV_DMABUF_RX_ARMED);
        if (lands) {
            dmabuf_engine.rx.batch.pending = 0;
            tbv_dmabuf_rx_watch(NULL);
            assert(notified == TBV_DMABUF_DONE && stopping_drops == stops_before);
        } else {
            jiffies += TBV_DMABUF_CANCEL_DRAIN_MS;
            tbv_dmabuf_rx_watch(NULL);
            assert(notified == -ECANCELED && stopping_drops == stops_before + 1);
        }
        assert(finishes == before + 1 && dmabuf_engine.rx.state == TBV_DMABUF_RX_CACHED);
        alive = true;
    }
    // Allocation/post failures finish a begun WR; missing lanes never begin one.
    m.token++; alloc_result = -ENOMEM;
    assert(tbv_dmabuf_control_request(&peer, &ctrl, &m) == -ENOMEM);
    alloc_result = 0; m.token++; post_result = -EIO;
    assert(tbv_dmabuf_control_request(&peer, &ctrl, &m) == -EIO);
    post_result = 0; m.token++; lane_available = false;
    unsigned int before = begins;
    assert(tbv_dmabuf_control_request(&peer, &ctrl, &m) == -ENOTCONN);
    assert(begins == before && !rail.refs && ctrl.refs == 1);
    lane_available = true;

    /* Pre-armed SEND credits. A finished GPU SEND arms the next WQE. */
    struct tbv_rail link_rail = { .peer = &peer };
    peer.dmabuf_control = &ctrl;
    dmabuf_engine.state = &state;
    int wq;
    dmabuf_engine.post_wq = &wq;
    struct tbv_dmabuf_wire s = { .op = TBV_DMABUF_PREPARE, .token = 100, .count = 1,
        .bytes = 10304, .hdr.length = 10304, .hdr.opcode = TBV_NATIVE_DATA_OP_SEND_IMM,
        .hdr.src_qp = 7, .hdr.dest_qp = 9, .hdr.psn = 40 };
    assert(tbv_dmabuf_control_request(&peer, &ctrl, &s) == TBV_DMABUF_READY);
    dmabuf_engine.rx.buffer.mr = &gpu_mr;
    dmabuf_engine.rx.batch.pending = 0;
    unsigned int n0 = notifies;
    assert(tbv_dmabuf_rx_reap_impl() == TBV_DMABUF_DONE);
    assert(dmabuf_engine.rx.state == TBV_DMABUF_RX_SPECULATIVE && notifies == n0 + 1);
    assert(last_notify.op == (TBV_DMABUF_CREDIT | TBV_DMABUF_REPLY) && !last_notify.status);
    assert(last_notify.hdr.psn == 41 && last_notify.credit == 1 && !dmabuf_engine.want_ctrl);
    assert(counts[CREDIT_GRANTED] == 1 && rail.refs == 1 && ctrl.refs == 2);
    /* STATUS for the finished WR still reports its cached result. */
    s.op = TBV_DMABUF_STATUS;
    assert(tbv_dmabuf_control_request(&peer, &ctrl, &s) == TBV_DMABUF_DONE);
    /* COMMIT with the credit adopts it; DONE follows its DMA. */
    struct tbv_dmabuf_wire c = last_notify;
    c.op = TBV_DMABUF_COMMIT | TBV_DMABUF_REPLY; c.token = 101; c.hdr.imm_data = 5;
    dmabuf_engine.rx.batch.pending = 1;
    unsigned int f0 = finishes;
    tbv_dmabuf_credit_commit(&peer, &c);
    assert(dmabuf_engine.rx.state == TBV_DMABUF_RX_ARMED && claims == 1);
    assert(counts[CREDIT_COMMITTED] == 1 && !dmabuf_engine.rx.credit_ctrl);
    assert(dmabuf_engine.rx.request.op == TBV_DMABUF_PREPARE && !dmabuf_engine.rx.request.credit);
    assert(peer.dmabuf_rx_token == 101);
    /* The next WQE is not posted yet: remember the stream, arm on post. */
    peek_result = -EAGAIN;
    dmabuf_engine.rx.batch.pending = 0;
    assert(tbv_dmabuf_rx_reap_impl() == TBV_DMABUF_DONE);
    assert(finishes == f0 + 1 && dmabuf_engine.rx.state == TBV_DMABUF_RX_CACHED);
    assert(dmabuf_engine.want_owner == &rail && dmabuf_engine.want_ctrl == &ctrl);
    tbv_dmabuf_recv_posted(&peer); assert(!queued);
    tbv_dmabuf_recv_posted(&rail); assert(queued == 1);
    peek_result = 0;
    tbv_dmabuf_credit_work(NULL);
    assert(dmabuf_engine.rx.state == TBV_DMABUF_RX_SPECULATIVE && !dmabuf_engine.want_owner);
    assert(last_notify.credit == 2 && last_notify.hdr.psn == 42);
    /* STATUS before COMMIT: the frames may be placed, COMMIT is in flight. */
    struct tbv_dmabuf_wire st = last_notify;
    st.op = TBV_DMABUF_STATUS; st.credit = 0; st.token = 104;
    assert(tbv_dmabuf_control_request(&peer, &ctrl, &st) == TBV_DMABUF_BUSY);
    assert(dmabuf_engine.rx.state == TBV_DMABUF_RX_SPECULATIVE);
    st.hdr.psn = 99;
    assert(tbv_dmabuf_control_request(&peer, &ctrl, &st) == -ENOENT);
    /* A PREPARE that raced the credit adopts it; READY names the credit. */
    struct tbv_dmabuf_wire p = last_notify;
    p.op = TBV_DMABUF_PREPARE; p.credit = 0; p.token = 102;
    assert(tbv_dmabuf_control_request(&peer, &ctrl, &p) == TBV_DMABUF_READY);
    assert(p.credit == 2 && counts[CREDIT_ADOPTED] == 1);
    assert(dmabuf_engine.rx.state == TBV_DMABUF_RX_ARMED && dmabuf_engine.rx.control == &ctrl);
    dmabuf_engine.rx.batch.pending = 0;
    assert(tbv_dmabuf_rx_reap_impl() == TBV_DMABUF_DONE);
    assert(dmabuf_engine.rx.state == TBV_DMABUF_RX_SPECULATIVE && last_notify.credit == 3);
    /* A PREPARE that cannot use it cancels; RELEASE plus filler frees the
     * rings without a CQE, and the retried PREPARE proceeds normally. */
    struct tbv_dmabuf_wire w = last_notify;
    w.op = TBV_DMABUF_PREPARE; w.credit = 0; w.token = 103;
    w.hdr.opcode = TBV_NATIVE_DATA_OP_RDMA_WRITE_IMM;
    assert(tbv_dmabuf_control_request(&peer, &ctrl, &w) == TBV_DMABUF_BUSY);
    assert(dmabuf_engine.rx.cancel_sent && last_notify.status == -ECANCELED);
    assert(counts[CREDIT_CANCELED] == 1);
    assert(tbv_dmabuf_control_request(&peer, &ctrl, &w) == TBV_DMABUF_BUSY);
    assert(counts[CREDIT_CANCELED] == 1);
    struct tbv_dmabuf_wire r = last_notify;
    r.op = TBV_DMABUF_RELEASE | TBV_DMABUF_REPLY; r.status = 0;
    dmabuf_engine.rx.batch.pending = 1;
    f0 = finishes;
    tbv_dmabuf_credit_commit(&peer, &r);
    assert(dmabuf_engine.rx.state == TBV_DMABUF_RX_SPECULATIVE && dmabuf_engine.rx.released);
    dmabuf_engine.rx.batch.pending = 0;
    assert(tbv_dmabuf_control_request(&peer, &ctrl, &w) == TBV_DMABUF_READY);
    assert(counts[CREDIT_RELEASED] == 1 && !counts[CREDIT_ABORTED] && finishes == f0 + 1);
    dmabuf_engine.rx.batch.pending = 0;
    assert(tbv_dmabuf_rx_reap_impl() == TBV_DMABUF_DONE);
    /* WRITE completions never arm a credit. */
    assert(dmabuf_engine.rx.state == TBV_DMABUF_RX_CACHED);
    /* A COMMIT for a credit we do not hold quarantines its lanes. */
    struct tbv_dmabuf_wire bad = c;
    bad.credit = 77; bad.token = 200;
    tbv_dmabuf_credit_commit(&peer, &bad);
    assert(counts[CREDIT_MISMATCH] == 1 && stops == 1);
    /* Destroying the QP behind an armed credit waits for RELEASE, then
     * quarantines the rails when the sender never fills them. */
    dmabuf_engine.rx.last = s;
    dmabuf_engine.rx.last.hdr.psn = 50;
    tbv_control_get(&ctrl);
    dmabuf_engine.want = dmabuf_engine.rx.last;
    dmabuf_engine.want_ctrl = &ctrl;
    tbv_dmabuf_credit_arm();
    assert(dmabuf_engine.rx.state == TBV_DMABUF_RX_SPECULATIVE);
    tbv_dmabuf_cancel_recv(&rail);
    assert(dmabuf_engine.rx.state == TBV_DMABUF_RX_CACHED && counts[CREDIT_ABORTED] == 1);
    /* A lost CREDIT notification never leaves descriptors nobody fills. */
    tbv_control_get(&ctrl);
    dmabuf_engine.want_ctrl = &ctrl;
    notify_result = -EIO;
    tbv_dmabuf_credit_arm();
    assert(dmabuf_engine.rx.state == TBV_DMABUF_RX_CACHED && counts[CREDIT_ABORTED] == 2);
    notify_result = 0;
    /* Disconnect drops a remembered stream; references balance. */
    tbv_control_get(&ctrl);
    dmabuf_engine.want_ctrl = &ctrl;
    dmabuf_engine.want_owner = &rail;
    tbv_dmabuf_disconnect(&link_rail);
    assert(!dmabuf_engine.want_ctrl && !dmabuf_engine.want_owner);
    assert(ctrl.refs == 1 && ref_gets == ref_puts && !rail.refs);
    puts("Verbs RX state: PASS (multi-batch, retry, cached result, empty barrier, errors, disconnect, ownership, credits)");
    return 0;
}
"""
run_c("rx-state", shim + state + counters + rx + mocks + body + cases)

# Run the same receive, failure and lifetime cases with an ordinary direct MR.
assert mocks.count(".mr = &gpu_mr") == 1
assert cases.count("dmabuf_engine.rx.buffer.mr = &gpu_mr") == 1
host_mocks=mocks.replace(".mr = &gpu_mr", ".host_mr = &gpu_mr")
host_cases=cases.replace("dmabuf_engine.rx.buffer.mr = &gpu_mr", "dmabuf_engine.rx.buffer.host_mr = &gpu_mr")
run_c("rx-state-host", shim + state + counters + rx + host_mocks + body + host_cases)
