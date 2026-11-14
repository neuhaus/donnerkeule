// SPDX-License-Identifier: GPL-2.0
/* Opt-in pushed DONE on XDomain, or a separate per-XDomain control tunnel.
 * In ring mode frame/event storage is pooled; callbacks only copy/repost.
 * XDomain mode allocates software transaction storage without extra rings.
 */
#define pr_fmt(fmt) "thunderbolt_ibverbs control: " fmt

#include <linux/debugfs.h>
#include <linux/dma-mapping.h>
#include <linux/kref.h>
#include <linux/module.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/sort.h>
#include <linux/thunderbolt.h>
#include <linux/timekeeping.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

#include "../proto/ctrl_txn.h"
#include "tbv.h"
#include "verbs_control.h"
#include "perf.h"

#define TBV_CTRL_TX_COUNT 32
#define TBV_CTRL_RX_COUNT 64
#define TBV_CTRL_PENDING_MAX 16
#define TBV_CTRL_PDF 2

static bool dmabuf_control_ring;
module_param(dmabuf_control_ring, bool, 0444);
MODULE_PARM_DESC(dmabuf_control_ring,
	"Use separate NHI rings for descriptor verbs PREPARE/READY and pushed DONE when both peers support it");

static bool dmabuf_push_done;
module_param(dmabuf_push_done, bool, 0444);
MODULE_PARM_DESC(dmabuf_push_done,
	"Push descriptor verbs DONE over XDomain without reserving data rings when both peers support it");

struct tbv_ctrl_frame {
	struct ring_frame frame;
	struct tbv_control *ctrl;
	struct list_head node;
	void *buf;
	dma_addr_t dma;
};

struct tbv_ctrl_event {
	struct list_head node;
	u8 data[TBV_CTRL_FRAME_SIZE];
};

struct tbv_ctrl_pending {
	struct list_head node;
	struct tbv_control *ctrl;
	struct tbv_dmabuf_wire key;
	struct completion reply, done;
	struct tbv_ctrl_txn txn;
	u32 reply_credit;
};

enum tbv_ctrl_counter {
	CTRL_TX, CTRL_RX, CTRL_BAD, CTRL_UNMATCHED, CTRL_POOL_FULL,
	CTRL_PREPARE, CTRL_STATUS, CTRL_DONE, CTRL_PING, CTRL_TX_NS, CTRL_RX_NS,
	CTRL_TX_STARVED, CTRL_RX_OVERRUN,
	CTRL_DONE_RX, CTRL_NOTIFY_FAILED,
	CTRL_COUNTERS,
};

static const char * const ctrl_counter_names[] = {
	"tx", "rx", "bad", "unmatched", "pool_full", "prepare", "status",
	"done", "ping", "tx_ns", "rx_work_ns", "tx_starved", "rx_overrun",
	"done_rx", "notify_failed",
};

/* Hardware storage exists only for the optional separate-ring transport.
 * The refcounted transaction tracker can outlive it after disconnect. */
struct tbv_control_ring {
	struct tbv_control *ctrl;
	struct tb_ring *tx, *rx;
	struct device *dev;
	struct list_head tx_free, event_free, events;
	struct work_struct rx_work;
	struct delayed_work progress_work;
	atomic_t tx_free_count;
	struct tbv_ctrl_frame tx_frames[TBV_CTRL_TX_COUNT];
	struct tbv_ctrl_frame rx_frames[TBV_CTRL_RX_COUNT];
	struct tbv_ctrl_event rx_events[TBV_CTRL_RX_COUNT];
};

struct tbv_control {
	struct tbv_peer *peer;
	struct tb_xdomain *xd;
	struct kref refs;
	struct tbv_control_ring *ring;
	struct mutex setup_lock;
	spinlock_t lock;
	struct list_head pending;
	wait_queue_head_t tx_wait;
	atomic64_t counters[CTRL_COUNTERS];
	int local_path, remote_path;
	int failure;
	u32 nr_pending;
	bool running, enabled, ready, xdomain;
};

bool tbv_control_enabled(void)
{
	return dmabuf_control_ring && tbv_dmabuf_verbs_enabled();
}

bool tbv_control_push_enabled(void)
{
	return dmabuf_push_done && tbv_dmabuf_verbs_enabled();
}

bool tbv_control_xdomain_required(const struct tbv_peer *peer)
{
	return tbv_ctrl_need_xdomain(tbv_control_enabled(),
				     READ_ONCE(peer->remote_control_ring),
				     tbv_control_push_enabled(),
				     READ_ONCE(peer->remote_push_done));
}

bool tbv_control_required(const struct tbv_peer *peer)
{
	return tbv_ctrl_need(tbv_control_enabled(),
			     READ_ONCE(peer->remote_control_ring),
			     tbv_control_push_enabled(),
			     READ_ONCE(peer->remote_push_done));
}

bool tbv_control_ready(const struct tbv_peer *peer)
{
	struct tbv_control *ctrl = READ_ONCE(peer->dmabuf_control);

	return ctrl && READ_ONCE(ctrl->ready) && !READ_ONCE(ctrl->failure) &&
		ctrl->xdomain == tbv_control_xdomain_required(peer);
}

static void tbv_ctrl_fail(struct tbv_control *ctrl, int status)
{
	struct tbv_ctrl_pending *p;
	unsigned long flags;

	spin_lock_irqsave(&ctrl->lock, flags);
	if (!ctrl->failure)
		ctrl->failure = status;
	ctrl->ready = false;
	list_for_each_entry(p, &ctrl->pending, node) {
		complete_all(&p->reply);
		complete_all(&p->done);
	}
	spin_unlock_irqrestore(&ctrl->lock, flags);
	wake_up_all(&ctrl->tx_wait);
}

static void tbv_ctrl_progress(struct work_struct *work)
{
	struct tbv_control_ring *ring = container_of(to_delayed_work(work),
						   struct tbv_control_ring, progress_work);
	struct tbv_control *ctrl = ring->ctrl;

	if (!READ_ONCE(ctrl->running))
		return;
	/* Completion interrupts can coalesce while another ring worker runs.
	 * Only the NHI worker consumes descriptors, including these kicks. */
	schedule_work(&ctrl->ring->tx->work);
	schedule_work(&ctrl->ring->rx->work);
	mod_delayed_work(system_highpri_wq, &ctrl->ring->progress_work, msecs_to_jiffies(1));
}

static int tbv_ctrl_send(struct tbv_control *ctrl, const struct tbv_dmabuf_wire *m)
{
	struct tbv_ctrl_frame *f;
	unsigned long flags;
	u64 start = ktime_get_ns();
	unsigned long deadline = jiffies + msecs_to_jiffies(1000);
	int ret;

retry:
	spin_lock_irqsave(&ctrl->lock, flags);
	if (!ctrl->running || ctrl->failure) {
		ret = ctrl->failure ?: -ENOTCONN;
		goto out;
	}
	if (list_empty(&ctrl->ring->tx_free)) {
		atomic64_inc(&ctrl->counters[CTRL_POOL_FULL]);
		atomic64_inc(&ctrl->counters[CTRL_TX_STARVED]);
		spin_unlock_irqrestore(&ctrl->lock, flags);
		schedule_work(&ctrl->ring->tx->work);
		if (time_after_eq(jiffies, deadline))
			return -ETIMEDOUT;
		wait_event_timeout(ctrl->tx_wait, atomic_read(&ctrl->ring->tx_free_count) ||
			READ_ONCE(ctrl->failure), msecs_to_jiffies(1));
		goto retry;
	}
	f = list_first_entry(&ctrl->ring->tx_free, struct tbv_ctrl_frame, node);
	list_del_init(&f->node);
	atomic_dec(&ctrl->ring->tx_free_count);
	ret = tbv_ctrl_frame_build(f->buf, TBV_CTRL_FRAME_SIZE, m);
	if (ret >= 0) {
		f->frame.size = TBV_CTRL_FRAME_SIZE;
		f->frame.sof = 1;
		f->frame.eof = TBV_CTRL_PDF;
		f->frame.flags = 0;
		dma_wmb();
		ret = tb_ring_tx(ctrl->ring->tx, &f->frame);
	}
	if (ret) {
		list_add_tail(&f->node, &ctrl->ring->tx_free);
		atomic_inc(&ctrl->ring->tx_free_count);
	} else {
		atomic64_inc(&ctrl->counters[CTRL_TX]);
	}
out:
	spin_unlock_irqrestore(&ctrl->lock, flags);
	atomic64_add(ktime_get_ns() - start, &ctrl->counters[CTRL_TX_NS]);
	return ret;
}

static bool tbv_ctrl_source_valid(struct tbv_peer *peer,
				  const struct tbv_dmabuf_wire *m)
{
	return !m->status && peer->xd->remote_uuid &&
		!memcmp(m->src_host, peer->remote_host_id, 16) &&
		!memcmp(m->dst_host, peer->state->native_host_id, 16) &&
		!memcmp(m->src_domain, peer->xd->remote_uuid, 16);
}

/* Called by both transports before their ordinary response matching. The
 * list lock also excludes close(), so early and duplicate DONE are safe. */
static void tbv_ctrl_reply(struct tbv_control *ctrl, const struct tbv_dmabuf_wire *m)
{
	struct tbv_ctrl_pending *p;
	unsigned long flags;
	bool found = false;

	if (tbv_ctrl_done_malformed(m)) {
		atomic64_inc(&ctrl->counters[CTRL_BAD]);
		/* A malformed notification does not establish remote placement.
		 * Let the normal STATUS recovery resolve it without poisoning a
		 * different active transaction on the same connection. */
		return;
	}
	spin_lock_irqsave(&ctrl->lock, flags);
	list_for_each_entry(p, &ctrl->pending, node) {
		if (!tbv_dmabuf_wire_same(&p->key, m, true))
			continue;
		found = true;
		if (m->op == (TBV_DMABUF_COMPLETE | TBV_DMABUF_REPLY))
			atomic64_inc(&ctrl->counters[CTRL_DONE_RX]);
		switch (tbv_ctrl_txn_apply(&p->txn, m)) {
		case TBV_CTRL_APPLY_DONE:
			tbv_perf_wire_mark(m, true, "done_received", m->status, 0);
			complete_all(&p->done);
			break;
		case TBV_CTRL_APPLY_REPLY:
			tbv_perf_wire_mark(m, true, "control_reply", m->status, m->op);
			complete_all(&p->reply);
			break;
		default:
			break;
		}
		break;
	}
	spin_unlock_irqrestore(&ctrl->lock, flags);
	if (!found)
		atomic64_inc(&ctrl->counters[CTRL_UNMATCHED]);
}

void tbv_control_handle_done(struct tbv_state *state, struct tb_xdomain *source,
			     const struct tbv_dmabuf_wire *m)
{
	struct tbv_peer *peer, *found = NULL;
	struct tbv_control *ctrl;

	/* A reply keeps the PREPARE's source orientation: this host/domain
	 * originated the request. On legacy kernels the domain identifies
	 * the physical link even when routes on two controllers are equal. */
	mutex_lock(&state->lock);
	list_for_each_entry(peer, &state->peers, node) {
		if (peer->backend != TBV_BACKEND_NATIVE || (source && source != peer->xd) ||
		    !tbv_control_xdomain_required(peer) || !peer->remote_host_known ||
		    !peer->xd->local_uuid ||
		    memcmp(m->src_host, state->native_host_id, 16) ||
		    memcmp(m->dst_host, peer->remote_host_id, 16) ||
		    memcmp(m->src_domain, peer->xd->local_uuid, 16))
			continue;
		refcount_inc(&peer->refcnt);
		found = peer;
		break;
	}
	mutex_unlock(&state->lock);
	if (!found)
		return;
	ctrl = READ_ONCE(found->dmabuf_control);
	if (ctrl && ctrl->xdomain && tbv_control_ready(found)) {
		atomic64_inc(&ctrl->counters[CTRL_RX]);
		if (m->op == (TBV_DMABUF_CREDIT | TBV_DMABUF_REPLY))
			tbv_dmabuf_credit_received(found, m);
		else
			tbv_ctrl_reply(ctrl, m);
	}
	tbv_peer_put(state, found);
}

static void tbv_ctrl_receive(struct tbv_control *ctrl, const void *buf)
{
	struct tbv_dmabuf_wire m;
	int ret;

	ret = tbv_ctrl_frame_parse(buf, TBV_CTRL_FRAME_SIZE, &m);
	if (ret) {
		atomic64_inc(&ctrl->counters[CTRL_BAD]);
		return;
	}
	atomic64_inc(&ctrl->counters[CTRL_RX]);
	if (m.op & TBV_DMABUF_REPLY) {
		tbv_ctrl_reply(ctrl, &m);
		return;
	}
	if (!tbv_ctrl_source_valid(ctrl->peer, &m) || m.op == TBV_DMABUF_COMPLETE) {
		atomic64_inc(&ctrl->counters[CTRL_BAD]);
		return;
	}
	if (m.op == TBV_DMABUF_PREPARE)
		atomic64_inc(&ctrl->counters[CTRL_PREPARE]);
	else if (m.op == TBV_DMABUF_STATUS)
		atomic64_inc(&ctrl->counters[CTRL_STATUS]);
	else if (m.op == TBV_DMABUF_PING)
		atomic64_inc(&ctrl->counters[CTRL_PING]);
	tbv_dmabuf_control_request(ctrl->peer, ctrl, &m);
	m.op |= TBV_DMABUF_REPLY;
	ret = tbv_ctrl_send(ctrl, &m);
	if (ret)
		tbv_ctrl_fail(ctrl, ret);
}

static void tbv_ctrl_rx_work(struct work_struct *work)
{
	struct tbv_control_ring *ring = container_of(work, struct tbv_control_ring, rx_work);
	struct tbv_control *ctrl = ring->ctrl;
	struct tbv_ctrl_event *event;
	unsigned long flags;

	for (;;) {
		u64 start;

		spin_lock_irqsave(&ctrl->lock, flags);
		if (!ctrl->running || list_empty(&ctrl->ring->events)) {
			spin_unlock_irqrestore(&ctrl->lock, flags);
			break;
		}
		event = list_first_entry(&ctrl->ring->events, struct tbv_ctrl_event, node);
		list_del_init(&event->node);
		spin_unlock_irqrestore(&ctrl->lock, flags);
		start = ktime_get_ns();
		tbv_ctrl_receive(ctrl, event->data);
		atomic64_add(ktime_get_ns() - start, &ctrl->counters[CTRL_RX_NS]);
		spin_lock_irqsave(&ctrl->lock, flags);
		list_add_tail(&event->node, &ctrl->ring->event_free);
		spin_unlock_irqrestore(&ctrl->lock, flags);
	}
}

static void tbv_ctrl_callback(struct tb_ring *ring, struct ring_frame *frame,
			      bool canceled)
{
	struct tbv_ctrl_frame *f = container_of(frame, struct tbv_ctrl_frame, frame);
	struct tbv_control *ctrl = f->ctrl;
	struct tbv_ctrl_event *event;
	unsigned long flags;
	int ret = 0;

	if (ring->is_tx) {
		spin_lock_irqsave(&ctrl->lock, flags);
		list_add_tail(&f->node, &ctrl->ring->tx_free);
		atomic_inc(&ctrl->ring->tx_free_count);
		spin_unlock_irqrestore(&ctrl->lock, flags);
		wake_up_all(&ctrl->tx_wait);
		if (canceled && READ_ONCE(ctrl->running))
			tbv_ctrl_fail(ctrl, -ECANCELED);
		return;
	}
	if (canceled || !READ_ONCE(ctrl->running))
		return;
	if (frame->size != TBV_CTRL_FRAME_SIZE ||
	    (frame->sof && frame->sof != 1) || frame->eof != TBV_CTRL_PDF ||
	    frame->flags & (RING_DESC_CRC_ERROR | RING_DESC_BUFFER_OVERRUN)) {
		atomic64_inc(&ctrl->counters[CTRL_BAD]);
		ret = -EBADMSG;
		goto fail;
	}
	dma_rmb();
	spin_lock_irqsave(&ctrl->lock, flags);
	if (list_empty(&ctrl->ring->event_free)) {
		atomic64_inc(&ctrl->counters[CTRL_POOL_FULL]);
		atomic64_inc(&ctrl->counters[CTRL_RX_OVERRUN]);
		ret = -ENOBUFS;
	} else {
		event = list_first_entry(&ctrl->ring->event_free, struct tbv_ctrl_event, node);
		list_del_init(&event->node);
		memcpy(event->data, f->buf, TBV_CTRL_FRAME_SIZE);
		list_add_tail(&event->node, &ctrl->ring->events);
	}
	frame->size = TBV_CTRL_FRAME_SIZE;
	frame->sof = 1;
	frame->eof = TBV_CTRL_PDF;
	frame->flags = 0;
	if (ctrl->running && !ret)
		ret = tb_ring_rx(ctrl->ring->rx, frame);
	spin_unlock_irqrestore(&ctrl->lock, flags);
	if (!ret)
		queue_work(system_highpri_wq, &ctrl->ring->rx_work);
fail:
	if (ret)
		tbv_ctrl_fail(ctrl, ret);
}

static void tbv_ctrl_release(struct kref *ref)
{
	struct tbv_control *ctrl = container_of(ref, struct tbv_control, refs);

	tb_xdomain_put(ctrl->xd);
	kfree(ctrl);
}

void tbv_control_get(struct tbv_control *ctrl)
{
	if (ctrl)
		kref_get(&ctrl->refs);
}

void tbv_control_put(struct tbv_control *ctrl)
{
	if (ctrl)
		kref_put(&ctrl->refs, tbv_ctrl_release);
}

/* Stop/drain all callbacks before releasing hardware. RX may retain a small
 * software tracker until a multi-link batch finishes after this peer goes
 * away; that final notification only observes failure and drops its ref. */
static void tbv_ctrl_free(struct tbv_control *ctrl)
{
	u32 i;

	if (!ctrl->ring)
		goto put_tracker;
	for (i = 0; i < TBV_CTRL_TX_COUNT; i++)
		if (ctrl->ring->tx_frames[i].buf)
			dma_free_coherent(ctrl->ring->dev, TBV_CTRL_FRAME_SIZE,
				ctrl->ring->tx_frames[i].buf, ctrl->ring->tx_frames[i].dma);
	for (i = 0; i < TBV_CTRL_RX_COUNT; i++)
		if (ctrl->ring->rx_frames[i].buf)
			dma_free_coherent(ctrl->ring->dev, TBV_CTRL_FRAME_SIZE,
				ctrl->ring->rx_frames[i].buf, ctrl->ring->rx_frames[i].dma);
	if (ctrl->ring->rx)
		tb_ring_free(ctrl->ring->rx);
	if (ctrl->local_path >= 0)
		tb_xdomain_release_out_hopid(ctrl->xd, ctrl->local_path);
	if (ctrl->ring->tx)
		tb_ring_free(ctrl->ring->tx);
	kfree(ctrl->ring);
	ctrl->ring = NULL;
put_tracker:
	tbv_control_put(ctrl);
}

static int tbv_ctrl_alloc(struct tbv_peer *peer)
{
	struct tbv_control *ctrl;
	bool xdomain = tbv_control_xdomain_required(peer);
	u32 i;
	int ret = -ENOMEM;

	if (peer->dmabuf_control_dead)
		return -ENOTCONN;
	if (peer->dmabuf_control)
		return peer->dmabuf_control->xdomain == xdomain ? 0 : -ESTALE;
	/* Hop 0 belongs to the configuration channel. Some AMD NHIs expose
	 * only hops 1 and 2; lanes=1 leaves a ring pair for this prototype. */
	if (!xdomain && peer->xd->tb->nhi->hop_count <=
	    (peer->state->cfg.requested.lanes_auto ? TBV_NATIVE_MAX_LANES :
	     peer->state->cfg.requested.lanes_max) + 1) {
		pr_warn_once("no spare NHI rings (%u total hops, %u data rails); reserve a pair with lanes=1\n",
			peer->xd->tb->nhi->hop_count, peer->nr_rails);
		return -ENOSPC;
	}
	ctrl = kzalloc(sizeof(*ctrl), GFP_KERNEL);
	if (!ctrl)
		return -ENOMEM;
	ctrl->peer = peer;
	ctrl->xd = tb_xdomain_get(peer->xd);
	kref_init(&ctrl->refs);
	ctrl->xdomain = xdomain;
	ctrl->local_path = -1;
	ctrl->remote_path = -1;
	mutex_init(&ctrl->setup_lock);
	spin_lock_init(&ctrl->lock);
	INIT_LIST_HEAD(&ctrl->pending);
	init_waitqueue_head(&ctrl->tx_wait);
	if (xdomain) {
		/* Software transaction storage only. Hop 0 already belongs to
		 * XDomain; no additional rings, paths or polling worker. */
		ctrl->running = true;
		ctrl->ready = true;
		WRITE_ONCE(peer->dmabuf_control, ctrl);
		return 0;
	}
	ctrl->ring = kzalloc(sizeof(*ctrl->ring), GFP_KERNEL);
	if (!ctrl->ring)
		goto fail;
	ctrl->ring->ctrl = ctrl;
	INIT_LIST_HEAD(&ctrl->ring->tx_free);
	INIT_LIST_HEAD(&ctrl->ring->event_free);
	INIT_LIST_HEAD(&ctrl->ring->events);
	INIT_WORK(&ctrl->ring->rx_work, tbv_ctrl_rx_work);
	INIT_DELAYED_WORK(&ctrl->ring->progress_work, tbv_ctrl_progress);
	atomic_set(&ctrl->ring->tx_free_count, TBV_CTRL_TX_COUNT);
	ctrl->ring->tx = tb_ring_alloc_tx(peer->xd->tb->nhi, -1, TBV_CTRL_RING_SIZE,
				    RING_FLAG_FRAME);
	if (!ctrl->ring->tx)
		goto fail;
	ctrl->local_path = tb_xdomain_alloc_out_hopid(peer->xd, -1);
	if (ctrl->local_path < 0) {
		ret = ctrl->local_path;
		goto fail;
	}
	/* No E2E on these AMD NHIs: bounded transactions and large permanently
	 * posted RX pools cover the control traffic of the serial payload path. */
	ctrl->ring->rx = tb_ring_alloc_rx(peer->xd->tb->nhi, -1, TBV_CTRL_RING_SIZE,
				    RING_FLAG_FRAME, 0, BIT(1), BIT(TBV_CTRL_PDF), NULL, NULL);
	if (!ctrl->ring->rx)
		goto fail;
	ctrl->ring->dev = tb_ring_dma_device(ctrl->ring->tx);
	if (!tbv_dma_device_ready(ctrl->ring->dev)) {
		ret = -ENODEV;
		goto fail;
	}
	for (i = 0; i < TBV_CTRL_TX_COUNT + TBV_CTRL_RX_COUNT; i++) {
		struct tbv_ctrl_frame *f = i < TBV_CTRL_TX_COUNT ?
			&ctrl->ring->tx_frames[i] : &ctrl->ring->rx_frames[i - TBV_CTRL_TX_COUNT];

		INIT_LIST_HEAD(&f->frame.list);
		INIT_LIST_HEAD(&f->node);
		f->ctrl = ctrl;
		f->buf = dma_alloc_coherent(ctrl->ring->dev, TBV_CTRL_FRAME_SIZE, &f->dma,
					   GFP_KERNEL);
		if (!f->buf)
			goto fail;
		f->frame.buffer_phy = f->dma;
		f->frame.size = TBV_CTRL_FRAME_SIZE;
		f->frame.sof = 1;
		f->frame.eof = TBV_CTRL_PDF;
		f->frame.callback = tbv_ctrl_callback;
		if (i < TBV_CTRL_TX_COUNT)
			list_add_tail(&f->node, &ctrl->ring->tx_free);
	}
	for (i = 0; i < TBV_CTRL_RX_COUNT; i++)
		list_add_tail(&ctrl->ring->rx_events[i].node, &ctrl->ring->event_free);
	ctrl->running = true;
	tb_ring_start(ctrl->ring->tx);
	tb_ring_start(ctrl->ring->rx);
	for (i = 0; i < TBV_CTRL_RX_COUNT; i++) {
		ret = tb_ring_rx(ctrl->ring->rx, &ctrl->ring->rx_frames[i].frame);
		if (ret) {
			WRITE_ONCE(ctrl->running, false);
			tb_ring_stop(ctrl->ring->rx);
			tb_ring_stop(ctrl->ring->tx);
			cancel_work_sync(&ctrl->ring->rx_work);
			goto fail;
		}
	}
	WRITE_ONCE(peer->dmabuf_control, ctrl);
	mod_delayed_work(system_highpri_wq, &ctrl->ring->progress_work, msecs_to_jiffies(1));
	return 0;
fail:
	tbv_ctrl_free(ctrl);
	return ret;
}

static void tbv_ctrl_fill_setup(struct tbv_control *ctrl, struct tbv_ctrl_setup *m,
			       u16 op)
{
	memset(m, 0, sizeof(*m));
	m->op = op;
	m->path = ctrl->local_path;
	memcpy(m->src_host, ctrl->peer->state->native_host_id, 16);
	memcpy(m->dst_host, ctrl->peer->remote_host_id, 16);
	memcpy(m->domain, ctrl->peer->xd->local_uuid, 16);
}

static bool tbv_ctrl_setup_source(struct tbv_peer *peer, const struct tbv_ctrl_setup *m)
{
	return peer->remote_host_known && peer->xd->remote_uuid &&
		!memcmp(m->src_host, peer->remote_host_id, 16) &&
		!memcmp(m->dst_host, peer->state->native_host_id, 16) &&
		!memcmp(m->domain, peer->xd->remote_uuid, 16);
}

int tbv_control_handle_setup(struct tbv_state *state, struct tb_xdomain *source,
			     const void *buf, size_t size)
{
	struct tbv_ctrl_setup m, reply;
	struct tbv_peer *peer, *found = NULL;
	struct tbv_control *ctrl;
	u8 response[TBV_CTRL_SETUP_SIZE];
	int ret;

	if (!tbv_control_enabled() || tbv_ctrl_setup_parse(buf, size, &m) ||
	    m.op & TBV_DMABUF_REPLY)
		return 0;
	mutex_lock(&state->lock);
	list_for_each_entry(peer, &state->peers, node) {
		if (peer->backend != TBV_BACKEND_NATIVE || (source && source != peer->xd) ||
		    !tbv_ctrl_setup_source(peer, &m) || !peer->remote_control_ring)
			continue;
		refcount_inc(&peer->refcnt);
		found = peer;
		break;
	}
	mutex_unlock(&state->lock);
	if (!found)
		return 1;
	peer = found;
	mutex_lock(&peer->dmabuf_control_lock);
	ret = m.status ? -EINVAL : tbv_ctrl_alloc(peer);
	ctrl = peer->dmabuf_control;
	if (!ret) {
		if (ctrl->remote_path >= 0 && ctrl->remote_path != m.path)
			ret = -ESTALE;
		else {
			ctrl->remote_path = m.path;
			if (m.op == TBV_CTRL_READY) {
				ret = ctrl->enabled && !ctrl->failure ? 0 : -EAGAIN;
				if (!ret)
					WRITE_ONCE(ctrl->ready, true);
			}
		}
	}
	/* Allocation failure cannot advertise a usable local path. The
	 * initiator times out and retries; no payload uses this channel yet. */
	if (ctrl) {
		tbv_ctrl_fill_setup(ctrl, &reply, m.op | TBV_DMABUF_REPLY);
		reply.status = ret;
		ret = tbv_ctrl_setup_build(response, sizeof(response), &reply, peer->xd->route);
	} else {
		ret = -ENOMEM;
	}
	mutex_unlock(&peer->dmabuf_control_lock);
	if (ret >= 0)
		tb_xdomain_response(peer->xd, response, sizeof(response), TB_CFG_PKG_XDOMAIN_RESP);
	tbv_peer_put(state, peer);
	return 1;
}

int tbv_control_setup(struct tbv_peer *peer)
{
	struct tbv_control *ctrl;
	struct tbv_ctrl_setup m, reply;
	u8 request[TBV_CTRL_SETUP_SIZE], response[TBV_CTRL_SETUP_SIZE];
	int ret, op;

	if (!tbv_control_required(peer))
		return 0;
	mutex_lock(&peer->dmabuf_control_lock);
	ret = tbv_ctrl_alloc(peer);
	ctrl = peer->dmabuf_control;
	mutex_unlock(&peer->dmabuf_control_lock);
	if (ret)
		return ret;
	if (ctrl->xdomain)
		return tbv_control_ready(peer) ? 0 : -ENOTCONN;
	mutex_lock(&ctrl->setup_lock);
	if (tbv_control_ready(peer)) {
		ret = 0;
		goto out;
	}
	for (op = TBV_CTRL_SETUP; op <= TBV_CTRL_READY; op++) {
		if (READ_ONCE(peer->dmabuf_control_dead)) {
			ret = -ENOTCONN;
			break;
		}
		tbv_ctrl_fill_setup(ctrl, &m, op);
		ret = tbv_ctrl_setup_build(request, sizeof(request), &m, peer->xd->route);
		if (ret < 0)
			break;
		memset(response, 0, sizeof(response));
		mutex_lock(&peer->control_lock);
		ret = tb_xdomain_request(peer->xd, request, sizeof(request), TB_CFG_PKG_XDOMAIN_REQ,
			response, sizeof(response), TB_CFG_PKG_XDOMAIN_RESP, 1000);
		mutex_unlock(&peer->control_lock);
		if (ret)
			break;
		ret = tbv_ctrl_setup_parse(response, sizeof(response), &reply);
		if (ret || reply.op != (op | TBV_DMABUF_REPLY) ||
		    !tbv_ctrl_setup_source(peer, &reply)) {
			ret = -EPROTO;
			break;
		}
		if (reply.status) {
			ret = reply.status;
			break;
		}
		mutex_lock(&peer->dmabuf_control_lock);
		if (peer->dmabuf_control_dead || ctrl->failure) {
			ret = ctrl->failure ?: -ENOTCONN;
		} else if (ctrl->remote_path >= 0 && ctrl->remote_path != reply.path) {
			ret = -ESTALE;
		} else if (op == TBV_CTRL_SETUP && !ctrl->enabled) {
			ret = tb_xdomain_alloc_in_hopid(peer->xd, reply.path);
			if (ret == reply.path) {
				ctrl->remote_path = reply.path;
				mutex_lock(&peer->control_lock);
				ret = tb_xdomain_enable_paths(peer->xd, ctrl->local_path,
					ctrl->ring->tx->hop, ctrl->remote_path, ctrl->ring->rx->hop);
				mutex_unlock(&peer->control_lock);
				if (!ret)
					ctrl->enabled = true;
				else
					tb_xdomain_release_in_hopid(peer->xd, reply.path);
			} else {
				if (ret >= 0)
					tb_xdomain_release_in_hopid(peer->xd, ret);
				ret = ret < 0 ? ret : -EBUSY;
			}
		} else if (op == TBV_CTRL_READY) {
			WRITE_ONCE(ctrl->ready, true);
		}
		mutex_unlock(&peer->dmabuf_control_lock);
		if (ret)
			break;
	}
	if (!ret)
		pr_info("peer %u ready: out=%d remote_out=%d tx=%d rx=%d\n",
			peer->peer_id, ctrl->local_path, ctrl->remote_path, ctrl->ring->tx->hop, ctrl->ring->rx->hop);
out:
	mutex_unlock(&ctrl->setup_lock);
	return ret;
}

void tbv_control_stop(struct tbv_peer *peer)
{
	struct tbv_control *ctrl;
	bool stop = false;

	mutex_lock(&peer->dmabuf_control_lock);
	peer->dmabuf_control_dead = true;
	ctrl = peer->dmabuf_control;
	if (ctrl && ctrl->running) {
		WRITE_ONCE(ctrl->running, false);
		tbv_ctrl_fail(ctrl, -ENOTCONN);
		if (ctrl->enabled) {
			mutex_lock(&peer->control_lock);
			tb_xdomain_disable_paths(peer->xd, ctrl->local_path,
				ctrl->ring->tx->hop, ctrl->remote_path, ctrl->ring->rx->hop);
			mutex_unlock(&peer->control_lock);
			tb_xdomain_release_in_hopid(peer->xd, ctrl->remote_path);
			ctrl->enabled = false;
		}
		stop = true;
	}
	mutex_unlock(&peer->dmabuf_control_lock);
	if (stop && ctrl->ring) {
		cancel_delayed_work_sync(&ctrl->ring->progress_work);
		if (ctrl->ring->tx)
			tb_ring_stop(ctrl->ring->tx);
		if (ctrl->ring->rx)
			tb_ring_stop(ctrl->ring->rx);
		cancel_work_sync(&ctrl->ring->rx_work);
	}
}

void tbv_control_destroy(struct tbv_peer *peer)
{
	struct tbv_control *ctrl = peer->dmabuf_control;

	if (!ctrl)
		return;
	tbv_control_stop(peer);
	WARN_ON(ctrl->nr_pending);
	WRITE_ONCE(peer->dmabuf_control, NULL);
	tbv_ctrl_free(ctrl);
}

struct tbv_ctrl_pending *tbv_control_open(struct tbv_peer *peer,
					 const struct tbv_dmabuf_wire *m)
{
	struct tbv_control *ctrl;
	struct tbv_ctrl_pending *p;
	unsigned long flags;
	int ret = 0;

	/* No network exchange in XDomain setup, so a just-published data
	 * rail can lazily create its software tracker before the first WR. */
	if (tbv_control_xdomain_required(peer) && !tbv_control_ready(peer)) {
		ret = tbv_control_setup(peer);
		if (ret)
			return ERR_PTR(ret);
	}
	if (!tbv_control_ready(peer))
		return ERR_PTR(-ENOTCONN);
	ctrl = READ_ONCE(peer->dmabuf_control);
	p = kzalloc(sizeof(*p), GFP_KERNEL);
	if (!p)
		return ERR_PTR(-ENOMEM);
	p->ctrl = ctrl;
	p->key = *m;
	init_completion(&p->reply);
	init_completion(&p->done);
	spin_lock_irqsave(&ctrl->lock, flags);
	if (!ctrl->ready || ctrl->failure)
		ret = ctrl->failure ?: -ENOTCONN;
	else if (ctrl->nr_pending >= TBV_CTRL_PENDING_MAX)
		ret = -EBUSY;
	else {
		list_add_tail(&p->node, &ctrl->pending);
		ctrl->nr_pending++;
	}
	spin_unlock_irqrestore(&ctrl->lock, flags);
	if (ret) {
		kfree(p);
		return ERR_PTR(ret);
	}
	return p;
}

void tbv_control_close(struct tbv_ctrl_pending *p)
{
	unsigned long flags;

	if (!p)
		return;
	spin_lock_irqsave(&p->ctrl->lock, flags);
	list_del(&p->node);
	p->ctrl->nr_pending--;
	spin_unlock_irqrestore(&p->ctrl->lock, flags);
	kfree(p);
}

static int tbv_ctrl_xdomain_exchange(struct tbv_peer *peer,
				     const struct tbv_dmabuf_wire *m, u32 timeout_ms,
				     u32 *credit)
{
	struct tbv_dmabuf_wire reply;
	u8 request[TBV_DMABUF_WIRE_SIZE], response[TBV_DMABUF_WIRE_SIZE] = {};
	u64 start;
	int ret;

	ret = tbv_dmabuf_wire_build(request, sizeof(request), m, peer->xd->route);
	if (ret < 0)
		return ret;
	start = tbv_perf_clock();
	mutex_lock(&peer->control_lock);
	tbv_perf_wire(m, true, "xdomain_lock", start, 0, m->op);
	start = tbv_perf_clock();
	ret = tb_xdomain_request(peer->xd, request, sizeof(request),
		TB_CFG_PKG_XDOMAIN_REQ, response, sizeof(response),
		TB_CFG_PKG_XDOMAIN_RESP, timeout_ms);
	mutex_unlock(&peer->control_lock);
	tbv_perf_wire(m, true, "xdomain_request", start, ret, m->op);
	if (ret)
		return ret;
	if (tbv_dmabuf_wire_parse(response, sizeof(response), &reply) ||
	    reply.op != (m->op | TBV_DMABUF_REPLY) ||
	    !tbv_dmabuf_wire_same(m, &reply, true))
		return -EPROTO;
	if (credit)
		*credit = reply.credit;
	return reply.status;
}

int tbv_control_xdomain_exchange(struct tbv_peer *peer,
				 const struct tbv_dmabuf_wire *m, u32 timeout_ms)
{
	return tbv_ctrl_xdomain_exchange(peer, m, timeout_ms, NULL);
}

int tbv_control_exchange(struct tbv_ctrl_pending *p,
			 const struct tbv_dmabuf_wire *m, u32 timeout_ms)
{
	struct tbv_control *ctrl = p->ctrl;
	unsigned long flags;
	u64 start;
	int ret;

	spin_lock_irqsave(&ctrl->lock, flags);
	reinit_completion(&p->reply);
	p->txn.expected = m->op;
	p->txn.replied = false;
	p->reply_credit = 0;
	ret = ctrl->failure;
	spin_unlock_irqrestore(&ctrl->lock, flags);
	if (ret)
		return ret;
	if (ctrl->xdomain) {
		atomic64_inc(&ctrl->counters[CTRL_TX]);
		return tbv_ctrl_xdomain_exchange(ctrl->peer, m, timeout_ms,
						 &p->reply_credit);
	}
	start = tbv_perf_clock();
	ret = tbv_ctrl_send(ctrl, m);
	tbv_perf_wire(m, true, "control_ring_submit", start, ret, m->op);
	if (ret)
		return ret;
	start = tbv_perf_clock();
	if (!wait_for_completion_timeout(&p->reply, msecs_to_jiffies(timeout_ms))) {
		tbv_perf_wire(m, true, "control_reply_wait", start, -ETIMEDOUT, m->op);
		return -ETIMEDOUT;
	}
	spin_lock_irqsave(&ctrl->lock, flags);
	ret = ctrl->failure ?: (p->txn.replied ? p->txn.status : -ENOTCONN);
	spin_unlock_irqrestore(&ctrl->lock, flags);
	tbv_perf_wire(m, true, "control_reply_wait", start, ret, m->op);
	return ret;
}

/* The credit a READY reply adopted; credits exist only in XDomain mode. */
u32 tbv_control_reply_credit(const struct tbv_ctrl_pending *p)
{
	return p ? p->reply_credit : 0;
}

struct tbv_control *tbv_control_of(const struct tbv_ctrl_pending *p)
{
	return p ? p->ctrl : NULL;
}

int tbv_control_wait_done(struct tbv_ctrl_pending *p, u32 timeout_ms)
{
	unsigned long flags;
	u64 start = tbv_perf_clock();
	int ret;

	wait_for_completion_timeout(&p->done, msecs_to_jiffies(timeout_ms));
	spin_lock_irqsave(&p->ctrl->lock, flags);
	ret = tbv_ctrl_txn_result(&p->txn, p->ctrl->failure);
	spin_unlock_irqrestore(&p->ctrl->lock, flags);
	tbv_perf_wire(&p->key, true, "done_wait", start, ret, 0);
	return ret;
}

/* Opt-in fault injection for qualification: module parameter dmabuf_fault drops, fails,
 * delays or duplicates the next pushed notifications of one type. It is
 * off until a rule is written and only touches what this host transmits. */
enum tbv_fault_kind {
	FAULT_CREDIT, FAULT_CANCEL, FAULT_COMMIT, FAULT_RELEASE, FAULT_DONE,
	FAULT_KINDS,
};

enum tbv_fault_action {
	FAULT_NONE, FAULT_DROP, FAULT_FAIL, FAULT_DELAY, FAULT_DUP,
};

static const char * const fault_kind_names[] = {
	"credit", "cancel", "commit", "release", "done",
};

static const char * const fault_action_names[] = {
	"none", "drop", "fail", "delay", "dup",
};

struct tbv_fault_rule {
	enum tbv_fault_action action;
	u32 skip, count, ms;
	u64 hits;
};

struct tbv_fault_late {
	struct list_head node;
	struct delayed_work work;
	struct tbv_control *ctrl;
	u8 packet[TBV_DMABUF_WIRE_SIZE];
};

static DEFINE_SPINLOCK(fault_lock);
static struct tbv_fault_rule fault_rules[FAULT_KINDS];
static LIST_HEAD(fault_late);
static struct workqueue_struct *fault_wq;
static atomic_t fault_armed = ATOMIC_INIT(0);

/* Consume one message of this kind: the action to apply, if its turn came.
 * skip messages pass first; count messages are then affected. */
static enum tbv_fault_action tbv_fault_take(enum tbv_fault_kind kind, u32 *ms)
{
	struct tbv_fault_rule *r = &fault_rules[kind];
	enum tbv_fault_action action = FAULT_NONE;
	unsigned long flags;

	if (!atomic_read(&fault_armed))
		return FAULT_NONE;
	spin_lock_irqsave(&fault_lock, flags);
	if (r->action != FAULT_NONE) {
		if (r->skip) {
			r->skip--;
		} else {
			action = r->action;
			*ms = r->ms;
			r->hits++;
			if (!--r->count) {
				r->action = FAULT_NONE;
				atomic_dec(&fault_armed);
			}
		}
	}
	spin_unlock_irqrestore(&fault_lock, flags);
	return action;
}

/* "<kind> <action> [count=N] [skip=N] [ms=N]", or "clear". */
static int tbv_fault_parse(char *line)
{
	struct tbv_fault_rule rule = { .count = 1 };
	char *word = strsep(&line, " \t\n");
	enum tbv_fault_kind kind;
	unsigned long flags;
	u32 i, value;

	if (!word)
		return -EINVAL;
	if (!strcmp(word, "clear")) {
		spin_lock_irqsave(&fault_lock, flags);
		for (i = 0; i < FAULT_KINDS; i++)
			memset(&fault_rules[i], 0, sizeof(fault_rules[i]));
		atomic_set(&fault_armed, 0);
		spin_unlock_irqrestore(&fault_lock, flags);
		return 0;
	}
	for (kind = 0; kind < FAULT_KINDS; kind++)
		if (!strcmp(word, fault_kind_names[kind]))
			break;
	word = strsep(&line, " \t\n");
	if (kind == FAULT_KINDS || !word)
		return -EINVAL;
	for (i = FAULT_DROP; i <= FAULT_DUP; i++)
		if (!strcmp(word, fault_action_names[i]))
			rule.action = i;
	if (!rule.action)
		return -EINVAL;
	while ((word = strsep(&line, " \t\n"))) {
		u32 *field;

		if (!*word)
			continue;
		if (!strncmp(word, "count=", 6))
			field = &rule.count;
		else if (!strncmp(word, "skip=", 5))
			field = &rule.skip;
		else if (!strncmp(word, "ms=", 3))
			field = &rule.ms;
		else
			return -EINVAL;
		if (kstrtou32(strchr(word, '=') + 1, 0, &value))
			return -EINVAL;
		*field = value;
	}
	if (!rule.count || rule.ms > 10000 || (rule.action == FAULT_DELAY && !rule.ms))
		return -EINVAL;
	spin_lock_irqsave(&fault_lock, flags);
	if (fault_rules[kind].action == FAULT_NONE)
		atomic_inc(&fault_armed);
	rule.hits = fault_rules[kind].hits;
	fault_rules[kind] = rule;
	spin_unlock_irqrestore(&fault_lock, flags);
	return 0;
}

static void tbv_fault_late_work(struct work_struct *work)
{
	struct tbv_fault_late *late = container_of(work, struct tbv_fault_late, work.work);
	struct tbv_control *ctrl = late->ctrl;
	unsigned long flags;

	spin_lock_irqsave(&fault_lock, flags);
	list_del(&late->node);
	spin_unlock_irqrestore(&fault_lock, flags);
	if (READ_ONCE(ctrl->running) && !READ_ONCE(ctrl->failure))
		tb_xdomain_response(ctrl->xd, late->packet, sizeof(late->packet),
				    TB_CFG_PKG_XDOMAIN_RESP);
	tbv_control_put(ctrl);
	kfree(late);
}

static void tbv_fault_send_late(struct tbv_control *ctrl, const u8 *packet, u32 ms)
{
	struct tbv_fault_late *late = fault_wq ? kzalloc(sizeof(*late), GFP_KERNEL) : NULL;
	unsigned long flags;

	if (!late)
		return;
	memcpy(late->packet, packet, sizeof(late->packet));
	late->ctrl = ctrl;
	tbv_control_get(ctrl);
	INIT_DELAYED_WORK(&late->work, tbv_fault_late_work);
	spin_lock_irqsave(&fault_lock, flags);
	list_add_tail(&late->node, &fault_late);
	spin_unlock_irqrestore(&fault_lock, flags);
	queue_delayed_work(fault_wq, &late->work, msecs_to_jiffies(ms));
}

static void tbv_fault_flush(void)
{
	struct tbv_fault_late *late, *next;
	LIST_HEAD(canceled);
	unsigned long flags;

	/* A cancelled entry is ours to free. One that already started
	 * unlinks itself under the lock, so stays on the list until then. */
	spin_lock_irqsave(&fault_lock, flags);
	list_for_each_entry_safe(late, next, &fault_late, node)
		if (cancel_delayed_work(&late->work))
			list_move(&late->node, &canceled);
	spin_unlock_irqrestore(&fault_lock, flags);
	list_for_each_entry_safe(late, next, &canceled, node) {
		tbv_control_put(late->ctrl);
		kfree(late);
	}
	/* Waits for a delayed send that already started. */
	if (fault_wq) {
		destroy_workqueue(fault_wq);
		fault_wq = NULL;
	}
}

/* One pushed notification on XDomain, subject to any armed fault. */
static int tbv_ctrl_respond(struct tbv_control *ctrl, enum tbv_fault_kind kind,
			    u8 *packet)
{
	enum tbv_fault_action action;
	u32 ms = 0;

	action = tbv_fault_take(kind, &ms);
	if (action == FAULT_DROP)
		return 0;
	if (action == FAULT_FAIL)
		return -EIO;
	if (action == FAULT_DELAY) {
		tbv_fault_send_late(ctrl, packet, ms);
		return 0;
	}
	if (action == FAULT_DUP && !ms)
		tb_xdomain_response(ctrl->xd, packet, TBV_DMABUF_WIRE_SIZE,
				    TB_CFG_PKG_XDOMAIN_RESP);
	else if (action == FAULT_DUP)
		tbv_fault_send_late(ctrl, packet, ms);
	return tb_xdomain_response(ctrl->xd, packet, TBV_DMABUF_WIRE_SIZE,
				   TB_CFG_PKG_XDOMAIN_RESP);
}

/* A module parameter, not debugfs: lockdown hides writable debugfs files. */
static int tbv_fault_set(const char *value, const struct kernel_param *kp)
{
	char *line = kstrdup(value, GFP_KERNEL);
	int ret;

	if (!line)
		return -ENOMEM;
	ret = tbv_fault_parse(line);
	kfree(line);
	return ret;
}

static int tbv_fault_get(char *buf, const struct kernel_param *kp)
{
	enum tbv_fault_kind kind;
	unsigned long flags;
	int size = 0;

	spin_lock_irqsave(&fault_lock, flags);
	for (kind = 0; kind < FAULT_KINDS; kind++) {
		struct tbv_fault_rule *r = &fault_rules[kind];

		size += scnprintf(buf + size, PAGE_SIZE - size,
				  "%s: action=%s skip=%u count=%u ms=%u hits=%llu\n",
				  fault_kind_names[kind], fault_action_names[r->action],
				  r->skip, r->count, r->ms, r->hits);
	}
	spin_unlock_irqrestore(&fault_lock, flags);
	return size;
}

static const struct kernel_param_ops tbv_fault_ops = {
	.set = tbv_fault_set,
	.get = tbv_fault_get,
};
module_param_cb(dmabuf_fault, &tbv_fault_ops, NULL, 0600);
MODULE_PARM_DESC(dmabuf_fault,
	"Test only: write '<credit|cancel|commit|release|done> <drop|fail|delay|dup> [count=N] [skip=N] [ms=N]' or 'clear' to fault pushed notifications this host sends; read the rules and hits");

/* Credits require pushed notifications on XDomain from both peers. */
bool tbv_control_credits(const struct tbv_control *ctrl)
{
	return ctrl && ctrl->xdomain && READ_ONCE(ctrl->running) &&
		!READ_ONCE(ctrl->failure) && tbv_dmabuf_credits_enabled() &&
		READ_ONCE(ctrl->peer->remote_dmabuf_credit);
}

/* One-way CREDIT, COMMIT or RELEASE. Like DONE it is an XDomain response,
 * consumed by the peer's protocol handler before core response matching. */
int tbv_control_notify(struct tbv_control *ctrl, const struct tbv_dmabuf_wire *m)
{
	u8 packet[TBV_DMABUF_WIRE_SIZE];
	int ret;

	if (!tbv_control_credits(ctrl))
		return -ENOTCONN;
	ret = tbv_dmabuf_wire_build(packet, sizeof(packet), m, ctrl->xd->route);
	if (ret >= 0)
		ret = tbv_ctrl_respond(ctrl, m->op == (TBV_DMABUF_COMMIT | TBV_DMABUF_REPLY) ?
				       FAULT_COMMIT :
				       m->op == (TBV_DMABUF_RELEASE | TBV_DMABUF_REPLY) ?
				       FAULT_RELEASE : m->status < 0 ? FAULT_CANCEL :
				       FAULT_CREDIT, packet);
	atomic64_inc(&ctrl->counters[ret ? CTRL_NOTIFY_FAILED : CTRL_TX]);
	return ret;
}

void tbv_control_done(struct tbv_control *ctrl, const struct tbv_dmabuf_wire *request,
		      int status)
{
	struct tbv_dmabuf_wire m = *request;
	u64 start = tbv_perf_clock();
	int ret;

	if (!ctrl)
		return;
	m.op = TBV_DMABUF_COMPLETE | TBV_DMABUF_REPLY;
	m.status = status;
	atomic64_inc(&ctrl->counters[CTRL_DONE]);
	if (ctrl->xdomain) {
		u8 packet[TBV_DMABUF_WIRE_SIZE];

		if (!READ_ONCE(ctrl->running) || READ_ONCE(ctrl->failure))
			return;
		ret = tbv_dmabuf_wire_build(packet, sizeof(packet), &m, ctrl->xd->route);
		if (ret >= 0)
			ret = tbv_ctrl_respond(ctrl, FAULT_DONE, packet);
		if (ret)
			/* Cached placement remains valid. The source can recover
			 * a dropped/unallocatable notification with STATUS. */
			atomic64_inc(&ctrl->counters[CTRL_NOTIFY_FAILED]);
		else
			atomic64_inc(&ctrl->counters[CTRL_TX]);
	} else {
		ret = tbv_ctrl_send(ctrl, &m);
		if (ret)
			tbv_ctrl_fail(ctrl, ret);
	}
	tbv_perf_wire(&m, false, "done_submit", start, ret, ctrl->xdomain);
}

void tbv_control_debugfs_show(struct seq_file *s, struct tbv_peer *peer)
{
	struct tbv_control *ctrl = peer->dmabuf_control;
	u32 i;

	seq_printf(s, "  control_ring: requested=%u remote=%u ready=%u\n",
		tbv_control_enabled(), peer->remote_control_ring,
		ctrl && !ctrl->xdomain && tbv_control_ready(peer));
	seq_printf(s, "  push_done: requested=%u remote=%u ready=%u\n",
		tbv_control_push_enabled(), peer->remote_push_done,
		ctrl && ctrl->xdomain && tbv_control_ready(peer));
	seq_printf(s, "  nhi_hop_count: %u\n", peer->xd->tb->nhi->hop_count);
	if (!ctrl)
		return;
	if (!ctrl->xdomain)
		seq_printf(s, "  control_paths: out=%d remote_out=%d tx=%d rx=%d failure=%d\n",
			ctrl->local_path, ctrl->remote_path, ctrl->ring->tx->hop, ctrl->ring->rx->hop,
			READ_ONCE(ctrl->failure));
	else
		seq_printf(s, "  push_done_failure: %d\n", READ_ONCE(ctrl->failure));
	for (i = 0; i < CTRL_COUNTERS; i++)
		seq_printf(s, "  control_%s: %lld\n", ctrl_counter_names[i],
			atomic64_read(&ctrl->counters[i]));
}

static DEFINE_MUTEX(ctrl_bench_lock);
static char ctrl_bench_result[512] = "No control benchmark run\n";
static atomic64_t ctrl_ping_token = ATOMIC64_INIT(0);
static struct tbv_state *ctrl_bench_state;

static int tbv_ctrl_sample_compare(const void *a, const void *b)
{
	u64 x = *(const u64 *)a, y = *(const u64 *)b;

	return x > y ? 1 : x < y ? -1 : 0;
}

static int tbv_ctrl_bench_show(struct seq_file *s, void *unused)
{
	mutex_lock(&ctrl_bench_lock);
	seq_puts(s, ctrl_bench_result);
	mutex_unlock(&ctrl_bench_lock);
	return 0;
}

static int tbv_ctrl_bench_open(struct inode *inode, struct file *file)
{
	return single_open(file, tbv_ctrl_bench_show, inode->i_private);
}

static int tbv_ctrl_bench_run(const char *input)
{
	struct tbv_state *state;
	struct tbv_peer *peer, *found = NULL;
	struct tbv_dmabuf_wire m = {}, reply;
	u8 request[TBV_DMABUF_WIRE_SIZE], response[TBV_DMABUF_WIRE_SIZE];
	struct tbv_ctrl_pending *pending;
	u64 *samples, sum = 0;
	u32 peer_id, count, n = 0;
	bool fast;
	int ret = 0;

	if (!tbv_dmabuf_verbs_enabled() || !input || strlen(input) > 64)
		return -EINVAL;
	ret = sscanf(input, "%u %u", &peer_id, &count);
	if (ret != 2 || !count || count > 100000)
		return -EINVAL;
	samples = kcalloc(count, sizeof(*samples), GFP_KERNEL);
	if (!samples)
		return -ENOMEM;
	mutex_lock(&ctrl_bench_lock);
	state = ctrl_bench_state;
	if (!state) {
		mutex_unlock(&ctrl_bench_lock);
		kfree(samples);
		return -ENOTCONN;
	}
	mutex_lock(&state->lock);
	list_for_each_entry(peer, &state->peers, node)
		if (peer->peer_id == peer_id && peer->backend == TBV_BACKEND_NATIVE &&
		    peer->remote_host_known && !list_empty(&peer->rails)) {
			refcount_inc(&peer->refcnt);
			found = peer;
			break;
		}
	mutex_unlock(&state->lock);
	if (!found) {
		mutex_unlock(&ctrl_bench_lock);
		kfree(samples);
		return -ENOTCONN;
	}
	peer = found;
	/* Pushed DONE leaves the PREPARE/PING roundtrip on XDomain. */
	fast = tbv_control_enabled() && READ_ONCE(peer->remote_control_ring);
	m.op = TBV_DMABUF_PING;
	m.count = 1;
	m.hdr.opcode = TBV_NATIVE_DATA_OP_SEND;
	m.hdr.flags = TBV_NATIVE_DATA_F_RAW_STREAM;
	m.hdr.src_qp = m.hdr.dest_qp = 1;
	memcpy(m.src_host, state->native_host_id, 16);
	memcpy(m.dst_host, peer->remote_host_id, 16);
	memcpy(m.src_domain, peer->xd->local_uuid, 16);
	ret = 0;
	for (n = 0; n < count; n++) {
		u64 start = ktime_get_ns();

		m.token = atomic64_inc_return(&ctrl_ping_token);
		if (fast) {
			pending = tbv_control_open(peer, &m);
			if (IS_ERR(pending)) {
				ret = PTR_ERR(pending);
				break;
			}
			ret = tbv_control_exchange(pending, &m, 1000);
			tbv_control_close(pending);
		} else {
			tbv_dmabuf_wire_build(request, sizeof(request), &m, peer->xd->route);
			memset(response, 0, sizeof(response));
			mutex_lock(&peer->control_lock);
			ret = tb_xdomain_request(peer->xd, request, sizeof(request),
				TB_CFG_PKG_XDOMAIN_REQ, response, sizeof(response),
				TB_CFG_PKG_XDOMAIN_RESP, 1000);
			mutex_unlock(&peer->control_lock);
			if (!ret && (tbv_dmabuf_wire_parse(response, sizeof(response), &reply) ||
				    reply.op != (m.op | TBV_DMABUF_REPLY) ||
				    !tbv_dmabuf_wire_same(&m, &reply, true)))
				ret = -EPROTO;
			else if (!ret)
				ret = reply.status;
		}
		if (ret)
			break;
		samples[n] = ktime_get_ns() - start;
		sum += samples[n];
		if (signal_pending(current)) {
			ret = -EINTR;
			n++;
			break;
		}
	}
	if (n) {
		sort(samples, n, sizeof(*samples), tbv_ctrl_sample_compare, NULL);
	scnprintf(ctrl_bench_result, sizeof(ctrl_bench_result),
			"peer=%u transport=%s samples=%u requested=%u status=%d mean_ns=%llu min_ns=%llu p50_ns=%llu p95_ns=%llu p99_ns=%llu max_ns=%llu\n",
			peer_id, fast ? "nhi" : "xdomain", n, count, ret, div_u64(sum, n),
			samples[0], samples[(n - 1) / 2], samples[(u64)(n - 1) * 95 / 100],
			samples[(u64)(n - 1) * 99 / 100], samples[n - 1]);
	} else {
		scnprintf(ctrl_bench_result, sizeof(ctrl_bench_result),
			"peer=%u transport=%s samples=0 requested=%u status=%d\n",
			peer_id, fast ? "nhi" : "xdomain", count, ret);
	}
	tbv_peer_put(state, peer);
	mutex_unlock(&ctrl_bench_lock);
	kfree(samples);
	return ret;
}

static int tbv_ctrl_bench_set(const char *value, const struct kernel_param *kp)
{
	return tbv_ctrl_bench_run(value);
}

static int tbv_ctrl_bench_get(char *buf, const struct kernel_param *kp)
{
	int size;

	mutex_lock(&ctrl_bench_lock);
	size = scnprintf(buf, PAGE_SIZE, "%s", ctrl_bench_result);
	mutex_unlock(&ctrl_bench_lock);
	return size;
}

static const struct kernel_param_ops tbv_ctrl_bench_ops = {
	.set = tbv_ctrl_bench_set,
	.get = tbv_ctrl_bench_get,
};
module_param_cb(dmabuf_control_bench, &tbv_ctrl_bench_ops, NULL, 0600);
MODULE_PARM_DESC(dmabuf_control_bench,
	"Write 'peer_id count' to measure bounded control RTT (1..100000 samples); read results, no GPU payload");

static const struct file_operations tbv_ctrl_bench_fops = {
	.owner = THIS_MODULE,
	.open = tbv_ctrl_bench_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

void tbv_control_debugfs_init(struct tbv_state *state, struct dentry *dir)
{
	mutex_lock(&ctrl_bench_lock);
	ctrl_bench_state = state;
	mutex_unlock(&ctrl_bench_lock);
	debugfs_create_file("dmabuf_control_bench", 0400, dir, state, &tbv_ctrl_bench_fops);
	fault_wq = alloc_workqueue("tbv_fault", WQ_UNBOUND, 1);
}

void tbv_control_debugfs_exit(void)
{
	char clear[] = "clear";

	mutex_lock(&ctrl_bench_lock);
	ctrl_bench_state = NULL;
	mutex_unlock(&ctrl_bench_lock);
	tbv_fault_parse(clear);
	tbv_fault_flush();
}
