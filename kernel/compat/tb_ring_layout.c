// SPDX-License-Identifier: GPL-2.0
#include <linux/stddef.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/thunderbolt.h>

_Static_assert(sizeof(struct tb_ring) == 184, "tb_ring size");
_Static_assert(offsetof(struct tb_ring, descriptors) == 32, "tb_ring.descriptors");
_Static_assert(offsetof(struct tb_ring, in_flight) == 64, "tb_ring.in_flight");
_Static_assert(__builtin_types_compatible_p(typeof(((struct tb_nhi *)0)->dev),
					    struct device *), "tb_nhi.dev");

int tbv_tb_ring_layout_fields(struct tb_ring *r);
int tbv_tb_ring_layout_fields(struct tb_ring *r)
{
	unsigned long flags;
	int v;

	spin_lock_irqsave(&r->lock, flags);
	v = r->hop + r->is_tx + r->running + !!r->start_poll + r->size +
		r->head + r->tail + !!(r->flags & RING_FLAG_FRAME) +
		!!r->descriptors + list_empty(&r->queue) +
		list_empty(&r->in_flight) + r->nhi->hop_count +
		READ_ONCE(r->nhi->going_away) + !!r->nhi->iobase +
		!!r->nhi->dev;
	spin_unlock_irqrestore(&r->lock, flags);
	return v;
}
