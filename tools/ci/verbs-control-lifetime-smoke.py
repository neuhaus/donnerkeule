#!/usr/bin/env python3
"""Check actual stop/free paths, including a tracker held by another link's RX."""
from smoke_support import ROOT, declaration, functions, run_c

source = (ROOT / "kernel/verbs_control.c").read_text()
layouts = declaration(source, r"^struct tbv_control \{[\s\S]*?^\};")
body = functions(source, ("tbv_ctrl_release", "tbv_control_get", "tbv_control_put",
                          "tbv_ctrl_free", "tbv_control_stop", "tbv_control_destroy"))
shim = r"""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t atomic64_t;
typedef int spinlock_t;
struct mutex { int unused; };
struct kref { int count; };
struct list_head { void *next, *prev; };
struct tb_xdomain { int unused; };
struct tbv_control;
struct tbv_peer { struct tbv_control *dmabuf_control; struct tb_xdomain *xd; bool dmabuf_control_dead; struct mutex dmabuf_control_lock; };
#define CTRL_COUNTERS 7
#define container_of(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))
#define WRITE_ONCE(value, next) ((value) = (next))
#define mutex_lock(p) ((void)(p))
#define mutex_unlock(p) ((void)(p))
#define WARN_ON(value) assert(!(value))
static unsigned int xd_puts, failures;
static void kfree(void *p) { free(p); }
static void tb_xdomain_put(struct tb_xdomain *xd) { xd_puts++; }
static void kref_get(struct kref *refs) { assert(refs->count > 0); refs->count++; }
static void kref_put(struct kref *refs, void (*release)(struct kref *)) { assert(refs->count > 0); if (!--refs->count) release(refs); }
static void tbv_ctrl_fail(struct tbv_control *ctrl, int status);
"""
mocks = r"""
static void tbv_ctrl_fail(struct tbv_control *ctrl, int status) { failures++; ctrl->failure = status; ctrl->ready = false; }
static struct tbv_control *tracker(struct tbv_peer *peer) {
    struct tbv_control *ctrl = calloc(1, sizeof(*ctrl)); assert(ctrl);
    ctrl->refs.count = 1; ctrl->peer = peer; ctrl->running = ctrl->ready = true;
    peer->dmabuf_control = ctrl;
    return ctrl;
}
"""
cases = r"""
int main(void) {
    struct tbv_peer peer = {0};
    struct tbv_control *ctrl = tracker(&peer);
    tbv_control_get(ctrl); // RX on another link retains the transaction tracker.
    tbv_control_stop(&peer); tbv_control_stop(&peer);
    assert(peer.dmabuf_control_dead && !ctrl->ready && !ctrl->running);
    tbv_control_destroy(&peer);
    assert(!peer.dmabuf_control && ctrl->refs.count == 1);
    assert(xd_puts == 0 && ctrl->failure == -107);
    tbv_control_put(ctrl); assert(xd_puts == 1);
    tbv_control_destroy(&peer); // Idempotent on an unpublished tracker.
    assert(failures == 1);
    struct tbv_peer peer2 = {0};
    ctrl = tracker(&peer2);
    tbv_ctrl_free(ctrl);
    assert(xd_puts == 2);
    puts("Verbs control lifetime: PASS (XDomain tracker, stop, RX-held ref, destroy)");
    return 0;
}
"""
# Linux ENOTCONN differs from Darwin; use the Linux value in this kernel shim.
run_c("ctrl-lifetime", shim + "\n#define ENOTCONN 107\n" + layouts + mocks + body + cases)
