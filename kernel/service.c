// SPDX-License-Identifier: GPL-2.0

#define pr_fmt(fmt) "thunderbolt_ibverbs: " fmt

#include <linux/err.h>
#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/thunderbolt.h>
#include <linux/uuid.h>

#include "tbv.h"
#include "verbs_control.h"

const uuid_t tbv_native_service_uuid =
	UUID_INIT(0x7c2c8f1e, 0x5b4d, 0x4a01, 0x9f, 0x3a,
		  0x2b, 0x8e, 0x6d, 0x4c, 0x1a, 0x07);

static struct tbv_state *tbv_service_state;

static const char * const tbv_native_protocol_keys[TBV_NATIVE_MAX_LANES] = {
	TBV_NATIVE_PROTOCOL_KEY,
	"tbverb1",
	"tbverb2",
	"tbverb3",
};

struct tbv_service_binding {
	struct tbv_peer *peer;
	struct tbv_rail *rail;
};

static const char *tbv_native_protocol_key(u32 lane)
{
	if (lane >= ARRAY_SIZE(tbv_native_protocol_keys))
		return NULL;

	return tbv_native_protocol_keys[lane];
}

static struct tb_property_dir *
tbv_service_create_dir_common(const uuid_t *uuid, u32 prtcid, u32 prtcvers,
			      u32 prtcrevs, u32 prtcstns)
{
	struct tb_property_dir *dir;
	int ret;

	dir = tb_property_create_dir(uuid);
	if (!dir)
		return ERR_PTR(-ENOMEM);

	ret = tb_property_add_immediate(dir, "prtcid", prtcid);
	ret = ret ?: tb_property_add_immediate(dir, "prtcvers", prtcvers);
	ret = ret ?: tb_property_add_immediate(dir, "prtcrevs", prtcrevs);
	ret = ret ?: tb_property_add_immediate(dir, "prtcstns", prtcstns);
	if (ret) {
		tb_property_free_dir(dir);
		return ERR_PTR(ret);
	}

	return dir;
}

static u32 tbv_service_native_lane(const struct tb_service_id *id)
{
	return (u32)id->driver_data;
}

static bool tbv_service_native_domain_allowed(const struct tbv_state *state,
					      const struct tb_xdomain *xd)
{
	int index = xd->tb->index;

	if (index < 0 || index >= 32)
		return state->native_domain_mask == ~0u;
	return state->native_domain_mask & BIT(index);
}

static u32 tbv_config_native_lane_count(const struct tbv_state *state)
{
	if (state->cfg.lanes_auto)
		return min_t(u32, 2, TBV_NATIVE_MAX_LANES);

	return state->cfg.lanes_max;
}

static u32 tbv_service_path_id(const struct tb_service *svc,
			       const struct tb_service_id *id)
{
	u32 lane;

	lane = tbv_service_native_lane(id);
	if (!lane)
		return svc->prtcid;

	return (svc->prtcid << 8) | lane;
}

static int tbv_service_probe(struct tb_service *svc,
			     const struct tb_service_id *id)
{
	struct tb_xdomain *xd = tb_service_parent(svc);
	struct tbv_service_binding *binding;
	struct tbv_rail_key key;
	struct tbv_peer *peer;
	struct tbv_rail *rail;
	u32 native_lane = tbv_service_native_lane(id);
	u32 path_id;
	int ret;

	if (!tbv_service_state)
		return -ENODEV;

	if (!tbv_service_state->native_data)
		return -ENODEV;

	if (native_lane >= tbv_config_native_lane_count(tbv_service_state))
		return -ENODEV;

	if (!tbv_service_native_domain_allowed(tbv_service_state, xd))
		return -ENODEV;

	binding = kzalloc(sizeof(*binding), GFP_KERNEL);
	if (!binding)
		return -ENOMEM;

	peer = tbv_peer_get_or_create(tbv_service_state, xd);
	if (IS_ERR(peer)) {
		ret = PTR_ERR(peer);
		goto err_free_binding;
	}

	path_id = tbv_service_path_id(svc, id);
	tbv_rail_key_init(&key, xd->route, xd->link, xd->depth, path_id);
	rail = tbv_peer_add_rail(peer, &key, native_lane);
	if (IS_ERR(rail)) {
		ret = PTR_ERR(rail);
		goto err_put_peer;
	}

	rail->link_speed = xd->link_speed;
	rail->link_width = xd->link_width;

	if (tbv_service_state->allocate_rings) {
		ret = tbv_path_alloc_rings(&rail->path, xd, -1);
		if (ret)
			goto err_remove_rail;

		if (!rail->path.tx_ring || !rail->path.rx_ring) {
			ret = -EIO;
			goto err_remove_rail;
		}

		pr_debug("allocated rings service id=%d native_lane=%u tx_hop=%d rx_hop=%d out_hop=%d\n",
			svc->id, native_lane,
			rail->path.tx_ring->hop,
			rail->path.rx_ring->hop,
			rail->path.local_transmit_path);

		if (tbv_service_state->start_rings) {
			ret = tbv_path_start_rings(&rail->path);
			if (ret)
				goto err_remove_rail;

			pr_debug("started rings service id=%d native_lane=%u tx_hop=%d rx_hop=%d\n",
				svc->id, native_lane,
				rail->path.tx_ring->hop,
				rail->path.rx_ring->hop);

			if (tbv_service_state->negotiate_native)
				tbv_native_control_queue_rail(tbv_service_state,
							      rail);
		}
	}

	binding->peer = peer;
	binding->rail = rail;
	tb_service_set_drvdata(svc, binding);
	pr_info("bound native service id=%d key=%s native_lane=%u route=0x%llx link_speed=%uGb/s width=0x%x rail=0x%x path_id=%u\n",
		svc->id, svc->key, native_lane,
		xd->route, xd->link_speed, xd->link_width,
		tbv_rail_key_hash(&key), path_id);
	return 0;

err_remove_rail:
	tbv_peer_remove_rail(rail);
err_put_peer:
	tbv_peer_put(tbv_service_state, peer);
err_free_binding:
	kfree(binding);
	return ret;
}

static void tbv_service_remove(struct tb_service *svc)
{
	struct tbv_service_binding *binding = tb_service_get_drvdata(svc);

	if (tbv_service_state && binding) {
		tbv_peer_remove_rail(binding->rail);
		tbv_peer_put(tbv_service_state, binding->peer);
	}
	tb_service_set_drvdata(svc, NULL);
	kfree(binding);
}

/*
 * Reboot/poweroff hook, as thunderbolt_net has. Without it the rings stay
 * allocated through device_shutdown() and the NHI's own shutdown warns about
 * every one of them. Stops new TX and the control ring, stops the data rings,
 * then runs the same path teardown as tbv_service_remove(): tunnel disabled,
 * frames unmapped, rings freed. tbv_path_destroy() is idempotent, so the later
 * remove finds nothing left to do. Never waits for verbs users: userspace is
 * already gone, and a hung shutdown needs a power cycle. If the data rings
 * cannot be stopped within the bounded wait, they are left for remove.
 */
static void tbv_service_shutdown(struct tb_service *svc)
{
	struct tbv_service_binding *binding = tb_service_get_drvdata(svc);
	struct tbv_rail *rail;
	int ret;

	if (!tbv_service_state || !binding)
		return;
	rail = binding->rail;
	mutex_lock(&tbv_service_state->lock);
	rail->removing = true;
	mutex_unlock(&tbv_service_state->lock);
	tbv_control_stop(binding->peer);
	tbv_native_control_cancel_rail(rail);
	ret = tbv_dmabuf_quiesce_rail(rail);
	if (!ret)
		tbv_path_destroy(&rail->path, binding->peer->xd);
	dev_info(&svc->dev, "shutdown: rings stopped%s, status %d\n",
		 ret ? "" : " and freed", ret);
}

static const struct tb_service_id tbv_service_ids[] = {
	{ TB_SERVICE(TBV_NATIVE_PROTOCOL_KEY, TBV_NATIVE_PRTCID),
	  .driver_data = 0 },
	{ TB_SERVICE("tbverb1", TBV_NATIVE_PRTCID), .driver_data = 1 },
	{ TB_SERVICE("tbverb2", TBV_NATIVE_PRTCID), .driver_data = 2 },
	{ TB_SERVICE("tbverb3", TBV_NATIVE_PRTCID), .driver_data = 3 },
	{ },
};
MODULE_DEVICE_TABLE(tbsvc, tbv_service_ids);

#ifdef TBV_TB_SERVICE_PROBE_WITHOUT_ID
static int tbv_service_probe_without_id(struct tb_service *svc)
{
	const struct tb_service_id *id;

	/*
	 * The new callback omits the matched ID. Recover the same table entry
	 * so driver_data continues to select the native lane.
	 */
	for (id = tbv_service_ids; id->match_flags; id++) {
		if ((id->match_flags & TBSVC_MATCH_PROTOCOL_KEY) &&
		    strcmp(id->protocol_key, svc->key))
			continue;
		if ((id->match_flags & TBSVC_MATCH_PROTOCOL_ID) &&
		    id->protocol_id != svc->prtcid)
			continue;
		if ((id->match_flags & TBSVC_MATCH_PROTOCOL_VERSION) &&
		    id->protocol_version != svc->prtcvers)
			continue;
		if ((id->match_flags & TBSVC_MATCH_PROTOCOL_REVISION) &&
		    id->protocol_revision != svc->prtcrevs)
			continue;
		return tbv_service_probe(svc, id);
	}
	return -ENODEV;
}
#endif

static struct tb_service_driver tbv_service_driver = {
	.driver = {
		.owner = THIS_MODULE,
		.name = TBV_DRV_NAME,
	},
#ifdef TBV_TB_SERVICE_PROBE_WITHOUT_ID
	.probe = tbv_service_probe_without_id,
#else
	.probe = tbv_service_probe,
#endif
	.remove = tbv_service_remove,
	.shutdown = tbv_service_shutdown,
	.id_table = tbv_service_ids,
};

static int tbv_register_native_dir(struct tbv_state *state, u32 lane,
				   u32 prtcstns)
{
	const char *key = tbv_native_protocol_key(lane);
	int ret;

	if (!key)
		return -EINVAL;

	state->native_dirs[lane] =
		tbv_service_create_dir_common(&tbv_native_service_uuid,
					      TBV_NATIVE_PRTCID,
					      TBV_NATIVE_PRTCVERS,
					      TBV_NATIVE_PRTCREVS,
					      prtcstns);
	if (IS_ERR(state->native_dirs[lane])) {
		ret = PTR_ERR(state->native_dirs[lane]);
		state->native_dirs[lane] = NULL;
		return ret;
	}

	ret = tb_register_property_dir(key, state->native_dirs[lane]);
	if (ret) {
		tb_property_free_dir(state->native_dirs[lane]);
		state->native_dirs[lane] = NULL;
	}

	return ret;
}

static void tbv_unregister_native_dirs(struct tbv_state *state)
{
	u32 lane;

	for (lane = 0; lane < TBV_NATIVE_MAX_LANES; lane++) {
		const char *key = tbv_native_protocol_key(lane);

		if (!state->native_dirs[lane])
			continue;

		tb_unregister_property_dir(key, state->native_dirs[lane]);
		tb_property_free_dir(state->native_dirs[lane]);
		state->native_dirs[lane] = NULL;
	}
	state->native_dir_count = 0;
}

static int tbv_register_native_dirs(struct tbv_state *state, u32 prtcstns)
{
	u32 count;
	u32 lane;
	int ret;

	if (!state->native_data)
		return 0;

	count = tbv_config_native_lane_count(state);

	if (!count || count > TBV_NATIVE_MAX_LANES) {
		pr_err("native lanes request %u exceeds supported maximum %u\n",
		       count, TBV_NATIVE_MAX_LANES);
		return -EINVAL;
	}

	for (lane = 0; lane < count; lane++) {
		ret = tbv_register_native_dir(state, lane, prtcstns);
		if (ret) {
			tbv_unregister_native_dirs(state);
			return ret;
		}
		state->native_dir_count = lane + 1;
	}

	pr_info("advertised %u native service%s\n", count,
		count == 1 ? "" : "s");
	return 0;
}

int tbv_services_start(struct tbv_state *state, bool bind_services,
		       const struct tbv_service_config *service_cfg)
{
	int ret;

	state->allocate_rings = service_cfg->allocate_rings;
	state->start_rings = service_cfg->start_rings;
	state->negotiate_native = service_cfg->negotiate_native;
	state->enable_tunnels = service_cfg->enable_tunnels;

	if (!bind_services) {
		pr_info("Thunderbolt service binding disabled\n");
		return 0;
	}

	tbv_service_state = state;

	ret = tbv_native_control_start(state);
	if (ret)
		goto err_clear;

	ret = tbv_register_native_dirs(state, service_cfg->native_prtcstns);
	if (ret)
		goto err_clear;

	ret = tb_register_service_driver(&tbv_service_driver);
	if (ret)
		goto err_unregister_native;

	state->services_registered = true;
	pr_info("Thunderbolt service binding enabled\n");
	return 0;

err_unregister_native:
	tbv_unregister_native_dirs(state);
err_clear:
	tbv_native_control_stop(state);
	tbv_service_state = NULL;
	return ret;
}

void tbv_services_stop(struct tbv_state *state)
{
	if (state->services_registered) {
		tb_unregister_service_driver(&tbv_service_driver);
		state->services_registered = false;
	}

	tbv_native_control_stop(state);
	tbv_unregister_native_dirs(state);

	tbv_service_state = NULL;
}
