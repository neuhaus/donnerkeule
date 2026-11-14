#!/usr/bin/env python3
"""Exercise the control-transaction plumbing and fault injection.

Compiles the actual C bodies with simulated lists, locks and completions. The
per-transaction decisions (capability formula, DONE/reply ordering, wire
matching) live in proto/ctrl_txn.h and are tested by ctrl-txn-smoke.c; this
keeps the kernel wrappers around them: the module-parameter capability
wrappers and tbv_control_ready, end-to-end reply matching through
tbv_ctrl_reply, close, fail/wake, and dmabuf_fault rules.
"""
import os
import shlex
import pathlib
import re
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "kernel/verbs_control.c").read_text()


def extract(pattern):
    matches = re.findall(pattern, SOURCE, re.MULTILINE)
    if len(matches) != 1:
        raise RuntimeError(f"Expected exactly one production declaration: {pattern}")
    return matches[0]


declarations = extract(r"^struct tbv_ctrl_pending \{[\s\S]*?^\};")
declarations += extract(r"^enum tbv_ctrl_counter \{[\s\S]*?^\};")
functions = "\n".join(
    extract(rf"^(?:static )?(?:void|int|bool) {name}\([\s\S]*?^\}}")
    for name in ("tbv_control_enabled", "tbv_control_push_enabled",
                 "tbv_control_xdomain_required", "tbv_control_required",
                 "tbv_control_ready", "tbv_ctrl_fail", "tbv_ctrl_reply",
                 "tbv_control_close", "tbv_control_wait_done")
)

fault_decl = "\n".join(extract(pattern) for pattern in (
    r"^enum tbv_fault_kind \{[\s\S]*?^\};", r"^enum tbv_fault_action \{[\s\S]*?^\};",
    r"^static const char \* const fault_kind_names\[\][\s\S]*?^\};",
    r"^static const char \* const fault_action_names\[\][\s\S]*?^\};",
    r"^struct tbv_fault_rule \{[\s\S]*?^\};",
))
fault_decl += "static spinlock_t fault_lock;\nstatic atomic_t fault_armed;\n"
fault_decl += "static struct tbv_fault_rule fault_rules[FAULT_KINDS];\n"
fault_decl += "\n".join(extract(rf"^static [^\n]*\b{name}\([^;{{]*?^\{{[\s\S]*?^\}}")
                       for name in ("tbv_fault_take", "tbv_fault_parse"))

shim = r"""
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "proto/ctrl_txn.h"
#include "tools/ci/perf-noop.h"
typedef uint16_t u16;
typedef uint32_t u32;
typedef int spinlock_t;
static bool dmabuf_control_ring, dmabuf_push_done, dmabuf_verbs;
static bool tbv_dmabuf_verbs_enabled(void) { return dmabuf_verbs; }
#define READ_ONCE(value) (value)
struct completion { unsigned int calls; };
struct list_head { struct list_head *next, *prev; };
#define container_of(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))
#define spin_lock_irqsave(lock, flags) do { (void)(lock); (flags) = 0; } while (0)
#define spin_unlock_irqrestore(lock, flags) do { (void)(lock); (void)(flags); } while (0)
#define complete_all(c) ((c)->calls++)
#define wake_up_all(w) ((void)(w))
#define atomic64_inc(c) (++*(c))
typedef int atomic_t;
#define atomic_read(a) (*(a))
#define atomic_inc(a) (++*(a))
#define atomic_dec(a) (--*(a))
#define atomic_set(a, v) (*(a) = (v))
#define DEFINE_SPINLOCK(name) spinlock_t name
static int kstrtou32(const char *s, unsigned int base, u32 *res) {
    char *end;
    unsigned long v;
    if (!*s) return -EINVAL;
    v = strtoul(s, &end, base);
    if (*end || v > 0xffffffffu) return -EINVAL;
    *res = v;
    return 0;
}
#define msecs_to_jiffies(ms) (ms)
#define kfree(p) free(p)
#define list_for_each_entry(pos, head, member) \
    for (struct list_head *cursor = (head)->next; \
         cursor != (head) && ((pos) = container_of(cursor, __typeof__(*(pos)), member), 1); \
         cursor = cursor->next)
static void list_del(struct list_head *node) {
    node->next->prev = node->prev;
    node->prev->next = node->next;
}
static unsigned long wait_for_completion_timeout(struct completion *c, unsigned long ms) {
    (void)ms;
    return c->calls;
}
"""

control = r"""
struct tbv_control {
    spinlock_t lock;
    struct list_head pending;
    uint64_t counters[CTRL_COUNTERS];
    u32 nr_pending;
    int failure, tx_wait;
    bool ready, xdomain;
};
struct tbv_peer {
    bool remote_control_ring, remote_push_done;
    struct tbv_control *dmabuf_control;
};
"""

cases = r"""
static struct tbv_ctrl_pending *open_pending(struct tbv_control *ctrl, uint64_t token) {
    struct tbv_ctrl_pending *p = calloc(1, sizeof(*p));
    assert(p);
    p->ctrl = ctrl;
    p->key.token = token;
    memset(p->key.src_host, 0x11, 16);
    memset(p->key.dst_host, 0x22, 16);
    memset(p->key.src_domain, 0x33, 16);
    p->key.count = 1;
    p->key.hdr.opcode = TBV_NATIVE_DATA_OP_RDMA_WRITE_IMM;
    p->key.hdr.flags = TBV_NATIVE_DATA_F_RAW_STREAM;
    p->key.hdr.src_qp = 1;
    p->key.hdr.dest_qp = 2;
    p->key.hdr.psn = 3;
    p->key.hdr.length = 8192;
    p->key.bytes = 4096;
    p->key.lanes[0].path = 8;
    p->txn.expected = TBV_DMABUF_PREPARE;
    p->node.prev = ctrl->pending.prev;
    p->node.next = &ctrl->pending;
    ctrl->pending.prev->next = &p->node;
    ctrl->pending.prev = &p->node;
    ctrl->nr_pending++;
    return p;
}
static struct tbv_dmabuf_wire done(const struct tbv_ctrl_pending *p, int status) {
    struct tbv_dmabuf_wire m = p->key;
    m.op = TBV_DMABUF_COMPLETE | TBV_DMABUF_REPLY;
    m.status = status;
    return m;
}
static void deliver(struct tbv_control *ctrl, const struct tbv_dmabuf_wire *m) {
    unsigned char packet[TBV_DMABUF_WIRE_SIZE];
    struct tbv_dmabuf_wire decoded;
    assert(tbv_dmabuf_wire_build(packet, sizeof(packet), m, 1) == 252);
    assert(tbv_dmabuf_wire_parse(packet, sizeof(packet), &decoded) == 0);
    tbv_ctrl_reply(ctrl, &decoded);
}
int main(void) {
    struct tbv_control ctrl = {0};
    struct tbv_peer peer = {0};
    struct tbv_ctrl_pending *p, *other;
    struct tbv_dmabuf_wire m, late;
    ctrl.pending.next = ctrl.pending.prev = &ctrl.pending;
    ctrl.ready = true;
    /* Module parameters and peer capabilities through the kernel wrappers;
     * a tracker built for the other transport is never ready. */
    for (unsigned int flags = 0; flags < 16; flags++) {
        bool ring = (flags & 3) == 3;
        bool push = (flags & 12) == 12 && !ring;
        dmabuf_control_ring = flags & 1;
        peer.remote_control_ring = flags & 2;
        dmabuf_push_done = flags & 4;
        peer.remote_push_done = flags & 8;
        dmabuf_verbs = true;
        assert(tbv_control_enabled() == (bool)(flags & 1));
        assert(tbv_control_push_enabled() == (bool)(flags & 4));
        assert(tbv_control_xdomain_required(&peer) == push);
        assert(tbv_control_required(&peer) == (ring || push));
        peer.dmabuf_control = &ctrl;
        ctrl.xdomain = push;
        assert(tbv_control_ready(&peer));
        ctrl.xdomain = !push;
        assert(!tbv_control_ready(&peer));
        ctrl.xdomain = push;
        ctrl.failure = -EIO;
        assert(!tbv_control_ready(&peer));
        ctrl.failure = 0;
        dmabuf_verbs = false;
        assert(!tbv_control_enabled() && !tbv_control_push_enabled());
        assert(!tbv_control_xdomain_required(&peer) && !tbv_control_required(&peer));
    }
    peer.dmabuf_control = NULL;
    assert(!tbv_control_ready(&peer));
    p = open_pending(&ctrl, 100);
    other = open_pending(&ctrl, 101);
    assert(ctrl.nr_pending == 2);
    m = done(p, TBV_DMABUF_DONE);
    late = m;
    deliver(&ctrl, &m); /* Dispatched to its own tracker only. */
    assert(p->done.calls == 1 && !other->done.calls);
    assert(ctrl.counters[CTRL_DONE_RX] == 1);
    assert(tbv_control_wait_done(p, 0) == TBV_DMABUF_DONE);
    m = p->key;
    m.op = TBV_DMABUF_PREPARE | TBV_DMABUF_REPLY;
    deliver(&ctrl, &m);
    assert(p->reply.calls == 1 && !other->reply.calls);
    /* End-to-end matching: any key field off by one reaches no tracker. */
    for (unsigned int field = 0; field < 8; field++) {
        uint64_t unmatched = ctrl.counters[CTRL_UNMATCHED];
        m = done(other, TBV_DMABUF_DONE);
        switch (field) {
        case 0: m.token++; break;
        case 1: m.src_host[0]++; break;
        case 2: m.dst_host[0]++; break;
        case 3: m.src_domain[0]++; break;
        case 4: m.hdr.psn++; break;
        case 5: m.offset += 4096; break;
        case 6: m.lanes[0].path++; break;
        case 7: m.hdr.rkey++; break;
        }
        deliver(&ctrl, &m);
        assert(ctrl.counters[CTRL_UNMATCHED] == unmatched + 1);
        assert(!other->done.calls && tbv_control_wait_done(other, 0) == TBV_DMABUF_BUSY);
    }
    tbv_control_close(p);
    assert(ctrl.nr_pending == 1);
    deliver(&ctrl, &late); /* Closed tracker cannot be touched again. */
    assert(ctrl.counters[CTRL_UNMATCHED] == 9 && !other->done.calls);
    m = done(other, TBV_DMABUF_BUSY); /* Malformed DONE is counted, not applied. */
    deliver(&ctrl, &m);
    assert(ctrl.counters[CTRL_BAD] == 1 && !other->done.calls);
    tbv_ctrl_fail(&ctrl, -ENOTCONN);
    assert(!ctrl.ready && other->reply.calls && other->done.calls);
    assert(tbv_control_wait_done(other, 0) == -ENOTCONN);
    tbv_control_close(other);
    assert(!ctrl.nr_pending && ctrl.pending.next == &ctrl.pending);
    /* Fault injection: skip, count, expiry, replacement, bad input. */
    {
        char drop[] = "commit drop count=2 skip=1", dup[] = "done dup ms=5";
        char bad1[] = "commit explode", bad2[] = "credit delay", bad3[] = "nope drop";
        char bad4[] = "release drop count=0", bad5[] = "release drop bogus=1";
        char clear[] = "clear";
        u32 ms = 0;
        assert(tbv_fault_take(FAULT_COMMIT, &ms) == FAULT_NONE && !fault_armed);
        assert(tbv_fault_parse(bad1) && tbv_fault_parse(bad2) && tbv_fault_parse(bad3));
        assert(tbv_fault_parse(bad4) && tbv_fault_parse(bad5) && !fault_armed);
        assert(!tbv_fault_parse(drop) && !tbv_fault_parse(dup) && fault_armed == 2);
        assert(tbv_fault_take(FAULT_COMMIT, &ms) == FAULT_NONE); /* skipped */
        assert(tbv_fault_take(FAULT_CREDIT, &ms) == FAULT_NONE); /* other kind */
        assert(tbv_fault_take(FAULT_COMMIT, &ms) == FAULT_DROP);
        assert(tbv_fault_take(FAULT_COMMIT, &ms) == FAULT_DROP && fault_armed == 1);
        assert(tbv_fault_take(FAULT_COMMIT, &ms) == FAULT_NONE);
        assert(tbv_fault_take(FAULT_DONE, &ms) == FAULT_DUP && ms == 5 && !fault_armed);
        assert(tbv_fault_take(FAULT_DONE, &ms) == FAULT_NONE);
        assert(fault_rules[FAULT_COMMIT].hits == 2 && fault_rules[FAULT_DONE].hits == 1);
        strcpy(drop, "commit drop count=2 skip=1");
        assert(!tbv_fault_parse(drop) && fault_armed == 1 && !tbv_fault_parse(clear) && !fault_armed);
        assert(tbv_fault_take(FAULT_COMMIT, &ms) == FAULT_NONE);
    }
    puts("Verbs control state: PASS (capability wrappers, ready, matching, dispatch, late DONE after close, malformed DONE, disconnect, fault rules)");
    return 0;
}
"""

with tempfile.TemporaryDirectory(prefix="tbv-control-state-") as directory:
    source = pathlib.Path(directory) / "smoke.c"
    binary = pathlib.Path(directory) / "smoke"
    source.write_text(shim + declarations + fault_decl + control + functions + cases)
    subprocess.run([
        *shlex.split(os.environ.get("CC", "cc")), "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-g",
        "-fsanitize=address,undefined", "-I", str(ROOT), str(source),
        "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True)
