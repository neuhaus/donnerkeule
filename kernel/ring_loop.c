// SPDX-License-Identifier: GPL-2.0
/*
 * Software loopback ring implementation for VM and lockdep/KASAN testing.
 * Built only when TBV_LOOPBACK=1.
 */
#define pr_fmt(fmt) "thunderbolt_ibverbs loopback: " fmt

#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/random.h>
#include <linux/slab.h>
#include <linux/thunderbolt.h>
#include <linux/workqueue.h>

#include "tbv.h"
#include "verbs_internal.h"

static bool loopback;
module_param(loopback, bool, 0444);
MODULE_PARM_DESC(loopback,
		 "Create two back-to-back software loopback native rails (TBV_LOOPBACK=1)");

static uint loopback_drop_pct;
module_param(loopback_drop_pct, uint, 0644);
MODULE_PARM_DESC(loopback_drop_pct,
		 "Drop percentage (0..100) for loopback TX frames");

static uint loopback_delay_us;
module_param(loopback_delay_us, uint, 0644);
MODULE_PARM_DESC(loopback_delay_us,
		 "Delivery delay in microseconds for loopback TX frames");

static bool loopback_reorder;
module_param(loopback_reorder, bool, 0644);
MODULE_PARM_DESC(loopback_reorder,
		 "Swap adjacent queued loopback TX frames when >=2 are pending");

struct tbv_loop_ring {
	struct tb_ring tb;
	spinlock_t lock;
	struct list_head queue;
	struct list_head done;
	struct work_struct work;
	struct tbv_loop_ring *peer;
	struct device *dev;
	bool running;
};

static struct platform_device *tbv_loop_pdev;
static struct tbv_peer *tbv_loop_peers[2];
static struct tbv_rail *tbv_loop_rails[2];

static struct tbv_loop_ring *to_loop_ring(struct tb_ring *ring)
{
	return container_of(ring, struct tbv_loop_ring, tb);
}

static void *tbv_loop_dma_to_virt(struct device *dev, dma_addr_t dma)
{
	(void)dev;
	return phys_to_virt((phys_addr_t)dma);
}

static void tbv_loop_tx_work(struct work_struct *work)
{
	struct tbv_loop_ring *tx = container_of(work, struct tbv_loop_ring, work);
	struct tbv_loop_ring *rx = tx->peer;
	unsigned long flags;

	for (;;) {
		struct ring_frame *tf = NULL;
		struct ring_frame *rf = NULL;
		bool drop = false;
		u32 len;

		spin_lock_irqsave(&tx->lock, flags);
		if (tx->running && !list_empty(&tx->queue)) {
			if (loopback_reorder && tx->queue.next->next != &tx->queue)
				tf = list_entry(tx->queue.next->next,
						struct ring_frame, list);
			else
				tf = list_first_entry(&tx->queue,
						      struct ring_frame, list);
			list_del_init(&tf->list);
		}
		spin_unlock_irqrestore(&tx->lock, flags);

		if (!tf)
			break;

		if (loopback_delay_us)
			udelay(min_t(uint, loopback_delay_us, 10000));

		if (loopback_drop_pct &&
		    get_random_u32_below(100) < min_t(uint, loopback_drop_pct, 100))
			drop = true;

		if (!drop && rx) {
			spin_lock_irqsave(&rx->lock, flags);
			if (rx->running && !list_empty(&rx->queue)) {
				rf = list_first_entry(&rx->queue,
						      struct ring_frame, list);
				list_del_init(&rf->list);
			}
			spin_unlock_irqrestore(&rx->lock, flags);

			if (rf) {
				len = tf->size ? tf->size : TB_MAX_FRAME_SIZE;
				if (len > TB_MAX_FRAME_SIZE)
					len = TB_MAX_FRAME_SIZE;
				memcpy(tbv_loop_dma_to_virt(rx->dev, rf->buffer_phy),
				       tbv_loop_dma_to_virt(tx->dev, tf->buffer_phy),
				       len);
				rf->size = tf->size;
				rf->sof = tf->sof;
				rf->eof = tf->eof;
				rf->flags = 0;
			}
		}

		if (tf->callback)
			tf->callback(&tx->tb, tf, false);
		if (rf && rf->callback)
			rf->callback(&rx->tb, rf, false);
	}
}

static struct tb_ring *tbv_loop_alloc_ring(int hop, int size, bool is_tx)
{
	struct tbv_loop_ring *r;

	r = kzalloc(sizeof(*r), GFP_KERNEL);
	if (!r)
		return NULL;

	r->tb.hop = hop;
	r->tb.size = size;
	r->tb.is_tx = is_tx;
	spin_lock_init(&r->lock);
	INIT_LIST_HEAD(&r->queue);
	INIT_LIST_HEAD(&r->done);
	INIT_WORK(&r->work, tbv_loop_tx_work);
	r->dev = &tbv_loop_pdev->dev;
	return &r->tb;
}

static struct tb_ring *tbv_loop_alloc_tx(struct tb_nhi *nhi, int hop, int size,
					  unsigned int flags)
{
	return tbv_loop_alloc_ring(hop < 0 ? 1 : hop, size, true);
}

static struct tb_ring *
tbv_loop_alloc_rx(struct tb_nhi *nhi, int hop, int size, unsigned int flags,
		    int e2e_tx_hop, u16 sof_mask, u16 eof_mask,
		    void (*start_poll)(void *), void *poll_data)
{
	return tbv_loop_alloc_ring(hop < 0 ? 1 : hop, size, false);
}

static void tbv_loop_free(struct tb_ring *ring)
{
	struct tbv_loop_ring *r;

	if (!ring)
		return;
	r = to_loop_ring(ring);
	cancel_work_sync(&r->work);
	kfree(r);
}

static void tbv_loop_start(struct tb_ring *ring)
{
	struct tbv_loop_ring *r = to_loop_ring(ring);
	unsigned long flags;

	spin_lock_irqsave(&r->lock, flags);
	r->running = true;
	spin_unlock_irqrestore(&r->lock, flags);
}

static void tbv_loop_stop(struct tb_ring *ring)
{
	struct tbv_loop_ring *r = to_loop_ring(ring);
	struct ring_frame *f, *tmp;
	LIST_HEAD(canceled);
	unsigned long flags;

	spin_lock_irqsave(&r->lock, flags);
	r->running = false;
	list_splice_init(&r->queue, &canceled);
	spin_unlock_irqrestore(&r->lock, flags);

	cancel_work_sync(&r->work);

	list_for_each_entry_safe(f, tmp, &canceled, list) {
		list_del_init(&f->list);
		if (f->callback)
			f->callback(ring, f, true);
	}
}

static int tbv_loop_tx(struct tb_ring *ring, struct ring_frame *frame)
{
	struct tbv_loop_ring *r = to_loop_ring(ring);
	unsigned long flags;

	spin_lock_irqsave(&r->lock, flags);
	if (!r->running) {
		spin_unlock_irqrestore(&r->lock, flags);
		return -ENOTCONN;
	}
	list_add_tail(&frame->list, &r->queue);
	spin_unlock_irqrestore(&r->lock, flags);

	queue_work(system_highpri_wq, &r->work);
	return 0;
}

static int tbv_loop_rx(struct tb_ring *ring, struct ring_frame *frame)
{
	struct tbv_loop_ring *r = to_loop_ring(ring);
	unsigned long flags;

	spin_lock_irqsave(&r->lock, flags);
	if (!r->running) {
		spin_unlock_irqrestore(&r->lock, flags);
		return -ENOTCONN;
	}
	list_add_tail(&frame->list, &r->queue);
	spin_unlock_irqrestore(&r->lock, flags);
	return 0;
}

static struct ring_frame *tbv_loop_poll(struct tb_ring *ring)
{
	struct tbv_loop_ring *r = to_loop_ring(ring);
	struct ring_frame *f = NULL;
	unsigned long flags;

	spin_lock_irqsave(&r->lock, flags);
	if (!list_empty(&r->done)) {
		f = list_first_entry(&r->done, struct ring_frame, list);
		list_del_init(&f->list);
	}
	spin_unlock_irqrestore(&r->lock, flags);
	return f;
}

static struct device *tbv_loop_dma_device(struct tb_ring *ring)
{
	return to_loop_ring(ring)->dev;
}

const struct tbv_ring_ops tbv_loop_ring_ops = {
	.alloc_tx = tbv_loop_alloc_tx,
	.alloc_rx = tbv_loop_alloc_rx,
	.free = tbv_loop_free,
	.start = tbv_loop_start,
	.stop = tbv_loop_stop,
	.tx = tbv_loop_tx,
	.rx = tbv_loop_rx,
	.poll = tbv_loop_poll,
	.dma_device = tbv_loop_dma_device,
};

int tbv_loopback_start(struct tbv_state *state)
{
	struct tbv_rail_key keys[2] = {
		{ .route = 0x1001, .path_id = 1 },
		{ .route = 0x1002, .path_id = 1 },
	};
	int i, ret;

	if (!loopback)
		return 0;

	tbv_loop_pdev = platform_device_register_simple("tbv_loopback", -1,
							NULL, 0);
	if (IS_ERR(tbv_loop_pdev)) {
		ret = PTR_ERR(tbv_loop_pdev);
		tbv_loop_pdev = NULL;
		return ret;
	}
	ret = dma_coerce_mask_and_coherent(&tbv_loop_pdev->dev,
					   DMA_BIT_MASK(64));
	if (ret)
		goto err_pdev;

	for (i = 0; i < 2; i++) {
		tbv_loop_peers[i] = tbv_peer_get_or_create(state, NULL);
		if (IS_ERR(tbv_loop_peers[i])) {
			ret = PTR_ERR(tbv_loop_peers[i]);
			tbv_loop_peers[i] = NULL;
			goto err_cleanup;
		}
		mutex_lock(&state->lock);
		memset(tbv_loop_peers[i]->remote_host_id, 0xaa + i,
		       sizeof(tbv_loop_peers[i]->remote_host_id));
		tbv_loop_peers[i]->remote_host_known = true;
		mutex_unlock(&state->lock);

		tbv_loop_rails[i] = tbv_peer_add_rail(tbv_loop_peers[i],
						      &keys[i], 0);
		if (IS_ERR(tbv_loop_rails[i])) {
			ret = PTR_ERR(tbv_loop_rails[i]);
			tbv_loop_rails[i] = NULL;
			goto err_cleanup;
		}
		tbv_loop_rails[i]->path.ring_ops = &tbv_loop_ring_ops;
		ret = tbv_path_alloc_rings(&tbv_loop_rails[i]->path, NULL, -1);
		if (ret)
			goto err_cleanup;
	}

	to_loop_ring(tbv_loop_rails[0]->path.tx_ring)->peer =
		to_loop_ring(tbv_loop_rails[1]->path.rx_ring);
	to_loop_ring(tbv_loop_rails[1]->path.tx_ring)->peer =
		to_loop_ring(tbv_loop_rails[0]->path.rx_ring);

	for (i = 0; i < 2; i++) {
		ret = tbv_path_start_rings(&tbv_loop_rails[i]->path);
		if (ret)
			goto err_cleanup;
		mutex_lock(&state->lock);
		tbv_loop_rails[i]->path.state = TBV_PATH_TUNNEL_ENABLED;
		tbv_loop_rails[i]->native_negotiated = true;
		tbv_loop_rails[i]->native_ready_sent = true;
		tbv_loop_rails[i]->native_remote_ready = true;
		tbv_loop_rails[i]->remote_rail_id =
			tbv_loop_rails[1 - i]->rail_id;
		tbv_loop_rails[i]->remote_tx_hop = 1;
		tbv_loop_rails[i]->remote_rx_hop = 1;
		tbv_path_set_remote_rx_capacity(&tbv_loop_rails[i]->path,
			tbv_loop_rails[1 - i]->path.cfg.rx_ring_size);
		mutex_unlock(&state->lock);
	}

	for (i = 0; i < 2; i++)
		tbv_ibdev_rail_event(state, tbv_loop_rails[i], true);

	pr_info("started 2 software loopback rails\n");
	return 0;

err_cleanup:
	tbv_loopback_stop(state);
	return ret;

err_pdev:
	platform_device_unregister(tbv_loop_pdev);
	tbv_loop_pdev = NULL;
	return ret;
}

void tbv_loopback_stop(struct tbv_state *state)
{
	int i;

	for (i = 0; i < 2; i++) {
		if (tbv_loop_rails[i]) {
			tbv_peer_remove_rail(tbv_loop_rails[i]);
			tbv_loop_rails[i] = NULL;
		}
		if (tbv_loop_peers[i]) {
			tbv_peer_put(state, tbv_loop_peers[i]);
			tbv_loop_peers[i] = NULL;
		}
	}
	if (tbv_loop_pdev) {
		platform_device_unregister(tbv_loop_pdev);
		tbv_loop_pdev = NULL;
	}
}
