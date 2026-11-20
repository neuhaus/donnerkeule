// SPDX-License-Identifier: GPL-2.0

#define pr_fmt(fmt) "thunderbolt_ibverbs: " fmt

#include <linux/errno.h>
#include <linux/moduleparam.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/thunderbolt.h>

#include "tbv.h"
#include "verbs_control.h"

#define TBV_NATIVE_E2E_AUTO	(-1)

static int native_e2e = TBV_NATIVE_E2E_AUTO;
module_param(native_e2e, int, 0444);
MODULE_PARM_DESC(native_e2e,
		 "Native Linux data ring E2E mode: -1 auto, 0 off, 1 on");

static bool tbv_native_e2e_auto_enabled(const struct tbv_peer *peer)
{
	const struct tb_xdomain *xd = peer ? peer->xd : NULL;
	struct device *dev;
	struct pci_dev *pdev;

	if (!xd || !xd->tb)
		return true;

	dev = xd->tb->dev.parent;
	if (!dev || !dev_is_pci(dev))
		return true;

	pdev = to_pci_dev(dev);
	return pdev->vendor != PCI_VENDOR_ID_AMD;
}

static bool tbv_native_e2e_enabled(const struct tbv_peer *peer)
{
	if (native_e2e >= 0)
		return native_e2e > 0;

	return tbv_native_e2e_auto_enabled(peer);
}

struct tbv_peer *tbv_peer_get_or_create(struct tbv_state *state,
					struct tb_xdomain *xd)
{
	struct tbv_peer *peer;
	struct tbv_peer *pos;

	peer = kzalloc(sizeof(*peer), GFP_KERNEL);
	if (!peer)
		return ERR_PTR(-ENOMEM);

	refcount_set(&peer->refcnt, 1);
	peer->state = state;
	peer->xd = tb_xdomain_get(xd);
	INIT_LIST_HEAD(&peer->rails);
	mutex_init(&peer->control_lock);
	mutex_init(&peer->dmabuf_control_lock);

	mutex_lock(&state->lock);
	if (xd) {
		list_for_each_entry(pos, &state->peers, node) {
			if (pos->xd != xd)
				continue;

			refcount_inc(&pos->refcnt);
			mutex_unlock(&state->lock);
			tb_xdomain_put(peer->xd);
			kfree(peer);
			pr_debug("peer %u reused refs=%u\n", pos->peer_id,
				 refcount_read(&pos->refcnt));
			return pos;
		}
	}

	ida_init(&peer->rail_ids);
	peer->peer_id = state->next_peer_id++;
	list_add_tail(&peer->node, &state->peers);
	mutex_unlock(&state->lock);

	pr_info("peer %u created backend=native\n", peer->peer_id);
	return peer;
}

void tbv_peer_put(struct tbv_state *state, struct tbv_peer *peer)
{
	bool free_peer;

	if (!peer)
		return;

	mutex_lock(&state->lock);
	free_peer = refcount_dec_and_test(&peer->refcnt);
	if (free_peer)
		list_del_init(&peer->node);
	mutex_unlock(&state->lock);

	if (!free_peer)
		return;

	if (peer->nr_rails)
		pr_warn("peer %u destroyed with %u live rails\n",
			peer->peer_id, peer->nr_rails);

	pr_info("peer %u destroyed backend=native\n", peer->peer_id);
	ida_destroy(&peer->rail_ids);
	tbv_control_destroy(peer);
	tb_xdomain_put(peer->xd);
	kfree(peer);
}

struct tbv_rail *tbv_peer_add_rail(struct tbv_peer *peer,
				   const struct tbv_rail_key *key,
				   u32 native_lane)
{
	struct tbv_path_config path_cfg;
	struct tbv_rail *rail;
	struct tbv_rail *pos;
	int rail_id;

	rail = kzalloc(sizeof(*rail), GFP_KERNEL);
	if (!rail)
		return ERR_PTR(-ENOMEM);

	rail_id = ida_alloc(&peer->rail_ids, GFP_KERNEL);
	if (rail_id < 0) {
		kfree(rail);
		return ERR_PTR(rail_id);
	}

	rail->key = *key;
	rail->rail_id = rail_id;
	rail->native_lane = native_lane;
	atomic_set(&rail->native_qp_bind_count, 0);
	refcount_set(&rail->refcnt, 1);
	init_completion(&rail->refs_zero);
	rail->active = true;
	rail->remote_transmit_path = -1;
	rail->remote_tx_hop = -1;
	rail->remote_rx_hop = -1;
	rail->native_last_error = 0;
	rail->native_tunnel_attempts = 0;
	rail->native_ready_sent = false;
	rail->native_remote_ready = false;
	tbv_native_control_init_rail(rail, peer);
	tbv_path_default_config(&path_cfg);
	if (tbv_native_e2e_enabled(peer)) {
		/*
		 * E2E is hardware flow control only; it does not retransmit
		 * lost frames. In auto mode AMD NHI uses the software data
		 * credit scheme alone because Strix Halo has reproduced TX
		 * completion wedges with multiple native E2E rings active.
		 */
		path_cfg.tx_flags |= RING_FLAG_E2E;
		path_cfg.rx_flags |= RING_FLAG_E2E;
		path_cfg.e2e = true;
	}
	tbv_path_init(&rail->path, &path_cfg, rail);

	mutex_lock(&peer->state->lock);
	list_for_each_entry(pos, &peer->rails, node) {
		int cmp = tbv_rail_key_cmp(key, &pos->key);

		if (!cmp) {
			mutex_unlock(&peer->state->lock);
			ida_free(&peer->rail_ids, rail->rail_id);
			kfree(rail);
			return ERR_PTR(-EEXIST);
		}
		if (cmp < 0) {
			list_add_tail(&rail->node, &pos->node);
			peer->nr_rails++;
			mutex_unlock(&peer->state->lock);
			return rail;
		}
	}

	list_add_tail(&rail->node, &peer->rails);
	peer->nr_rails++;
	mutex_unlock(&peer->state->lock);
	return rail;
}

void tbv_peer_remove_rail(struct tbv_rail *rail)
{
	struct tbv_peer *peer;

	if (!rail)
		return;

	peer = rail->peer;

	/*
	 * Mark the rail as removing but leave it on peer->rails. Selectors
	 * (tbv_select_native_data_path_for_qp_locked et al.) all honor
	 * removing=true so no new TX targets pick this rail. Keeping the
	 * rail visible while ib_unregister_device() drains in-flight verbs
	 * callbacks means debugfs, native control, and any other observer
	 * sees a consistent topology snapshot until teardown actually
	 * completes -- there is never a window where a rail "exists but
	 * isn't reachable through peer->rails".
	 */
	mutex_lock(&peer->state->lock);
	rail->removing = true;
	mutex_unlock(&peer->state->lock);
	tbv_control_stop(peer);

	/*
	 * Tear down the per-rail ib_device (if any) before the path. Any QPs
	 * pinned to this rail hold a rail refcount, so ib_unregister_device's
	 * destroy_qp callbacks must complete before wait_for_completion
	 * (refs_zero) can return. This serializes data-path cleanup with
	 * verbs lifecycle and removes any chance of post_send racing
	 * tbv_path_destroy.
	 */
	tbv_dmabuf_disconnect(rail);
	tbv_ibdev_rail_event(peer->state, rail, false);

	tbv_native_control_cancel_rail(rail);
	tbv_rail_put(rail);
	wait_for_completion(&rail->refs_zero);

	/*
	 * All QPs that held a ref on this rail have now been destroyed
	 * (their refs were dropped in tbv_destroy_qp); it is safe to
	 * unlink the rail from peer->rails and free its path.
	 */
	mutex_lock(&peer->state->lock);
	if (!list_empty(&rail->node)) {
		list_del_init(&rail->node);
		if (peer->nr_rails)
			peer->nr_rails--;
	}
	mutex_unlock(&peer->state->lock);

	tbv_path_destroy(&rail->path, peer->xd);
	ida_free(&peer->rail_ids, rail->rail_id);
	kfree(rail);
}

void tbv_rail_put(struct tbv_rail *rail)
{
	if (refcount_dec_and_test(&rail->refcnt))
		complete(&rail->refs_zero);
}
