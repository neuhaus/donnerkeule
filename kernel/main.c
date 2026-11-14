// SPDX-License-Identifier: GPL-2.0
/*
 * Release-track Thunderbolt/USB4 RDMA verbs module.
 */

#define pr_fmt(fmt) "thunderbolt_ibverbs: " fmt

#include <linux/init.h>
#include <linux/module.h>
#include <linux/string.h>

#include "tbv.h"

static char *compat = "auto";
module_param(compat, charp, 0444);
MODULE_PARM_DESC(compat, "Deprecated and ignored: auto, off");

static char *profile = "auto";
module_param(profile, charp, 0444);
MODULE_PARM_DESC(profile, "Deprecated and ignored: auto, linux_perf, mixed");

static char *tbnet = "auto";
module_param(tbnet, charp, 0444);
MODULE_PARM_DESC(tbnet, "Deprecated and ignored by the kernel: auto, allow, prefer_rdma, block; NixOS uses thunderboltNetPolicy");

static char *tbnet_identity;
module_param(tbnet_identity, charp, 0444);
MODULE_PARM_DESC(tbnet_identity, "Deprecated and ignored; accepted for one release");

static char *lanes = "auto";
module_param(lanes, charp, 0444);
MODULE_PARM_DESC(lanes, "Lane request: auto, N, or MIN-MAX");

static bool bind_services;
module_param(bind_services, bool, 0444);
MODULE_PARM_DESC(bind_services,
		 "Register Thunderbolt service drivers and advertise services");

static uint native_prtcstns;
module_param(native_prtcstns, uint, 0444);
MODULE_PARM_DESC(native_prtcstns,
		 "Protocol settings advertised by the native Linux service");

static bool allocate_rings;
module_param(allocate_rings, bool, 0444);
MODULE_PARM_DESC(allocate_rings,
		 "Allocate Thunderbolt rings on service probe without enabling paths");

static bool start_rings;
module_param(start_rings, bool, 0444);
MODULE_PARM_DESC(start_rings,
		 "Start allocated Thunderbolt rings without enabling paths");

static bool negotiate_native;
module_param(negotiate_native, bool, 0444);
MODULE_PARM_DESC(negotiate_native,
		 "Send a native HELLO after ring start without enabling paths");

static bool enable_tunnels;
module_param(enable_tunnels, bool, 0444);
MODULE_PARM_DESC(enable_tunnels,
		 "Enable negotiated Thunderbolt paths after native HELLO");

static bool native_data = true;
module_param(native_data, bool, 0444);
MODULE_PARM_DESC(native_data,
		 "Allow native Linux peers to allocate rings and enable data paths");

static bool native_fragment_striping;
module_param(native_fragment_striping, bool, 0444);
MODULE_PARM_DESC(native_fragment_striping,
		 "Stripe native Linux SEND fragments across active rails");

static bool native_write_striping;
module_param(native_write_striping, bool, 0444);
MODULE_PARM_DESC(native_write_striping,
		 "Split native RDMA WRITEs into one contiguous block per active rail (both peers; implies native_fragment_striping)");

static unsigned int native_write_stripe_min_bytes = 65536;
module_param(native_write_stripe_min_bytes, uint, 0444);
MODULE_PARM_DESC(native_write_stripe_min_bytes,
		 "Smallest native RDMA WRITE split across rails; smaller ones stay on the QP's rail");

static unsigned int native_domain_mask = ~0u;
module_param(native_domain_mask, uint, 0444);
MODULE_PARM_DESC(native_domain_mask,
		 "Bitmask of Thunderbolt domain indices whose links carry native rails (default: all)");

static bool register_verbs;
module_param(register_verbs, bool, 0444);
MODULE_PARM_DESC(register_verbs,
		 "Register libibverbs devices for native rails");

static struct tbv_state tbv_driver_state;

static int __init tbv_init(void)
{
	char lanes_desc[32];
	struct tbv_service_config service_cfg;
	struct tbv_config cfg;
	int ret;

	ret = tbv_config_parse(&cfg, compat, profile, tbnet, lanes);
	if (ret)
		return ret;
	if (tbnet_identity)
		pr_warn("tbnet_identity=%s is deprecated and ignored\n",
			tbnet_identity);

	tbv_path_init_optional_symbols();
	if (tbv_dmabuf_verbs_enabled() &&
	    (!register_verbs || !bind_services || !allocate_rings || !start_rings ||
	     !negotiate_native || !enable_tunnels || !native_data)) {
		pr_err("dmabuf_verbs requires all path setup options and register_verbs=1\n");
		ret = -EINVAL;
		goto err_path_symbols;
	}

	if (start_rings && !allocate_rings) {
		pr_err("start_rings=1 requires allocate_rings=1\n");
		ret = -EINVAL;
		goto err_path_symbols;
	}

	if (negotiate_native && !start_rings) {
		pr_err("negotiate_native=1 requires start_rings=1\n");
		ret = -EINVAL;
		goto err_path_symbols;
	}

	if (enable_tunnels && !start_rings) {
		pr_err("enable_tunnels=1 requires start_rings=1\n");
		ret = -EINVAL;
		goto err_path_symbols;
	}

	if (enable_tunnels && !negotiate_native) {
		pr_err("enable_tunnels=1 requires negotiate_native=1\n");
		ret = -EINVAL;
		goto err_path_symbols;
	}

	ret = tbv_core_init(&tbv_driver_state, &cfg);
	if (ret)
		goto err_path_symbols;
	/*
	 * Block-striped writes advance the receive PSN out of arrival order, so
	 * SENDs on the same QP need the reordering receive path too.
	 */
	tbv_driver_state.native_fragment_striping = native_fragment_striping ||
						    native_write_striping;
	tbv_driver_state.native_write_striping = native_write_striping;
	tbv_driver_state.native_write_stripe_min_bytes =
		native_write_stripe_min_bytes;
	tbv_driver_state.native_domain_mask = native_domain_mask;
	tbv_driver_state.native_data = native_data;

	service_cfg.native_prtcstns = native_prtcstns;
	service_cfg.allocate_rings = allocate_rings;
	service_cfg.start_rings = start_rings;
	service_cfg.negotiate_native = negotiate_native;
	service_cfg.enable_tunnels = enable_tunnels;

	ret = tbv_dmabuf_start(&tbv_driver_state);
	if (ret) {
		tbv_core_exit(&tbv_driver_state);
		goto err_path_symbols;
	}
	ret = tbv_services_start(&tbv_driver_state, bind_services,
				 &service_cfg);
	if (ret) {
		tbv_dmabuf_stop();
		tbv_core_exit(&tbv_driver_state);
		goto err_path_symbols;
	}

	ret = tbv_ibdev_start(&tbv_driver_state, register_verbs);
	if (ret) {
		tbv_services_stop(&tbv_driver_state);
		tbv_dmabuf_stop();
		tbv_core_exit(&tbv_driver_state);
		goto err_path_symbols;
	}

	if (cfg.lanes_auto)
		strscpy(lanes_desc, "auto", sizeof(lanes_desc));
	else if (cfg.lanes_min == cfg.lanes_max)
		snprintf(lanes_desc, sizeof(lanes_desc), "%u",
			 cfg.lanes_min);
	else
		snprintf(lanes_desc, sizeof(lanes_desc), "%u-%u",
			 cfg.lanes_min, cfg.lanes_max);

	pr_info("loaded lanes=%s native_control=%s native_data=%u native_fragment_striping=%u\n",
		lanes_desc,
		tbv_native_control_mode_name(&tbv_driver_state),
		native_data,
		native_fragment_striping);

	return 0;

err_path_symbols:
	tbv_path_exit_optional_symbols();
	return ret;
}

static void __exit tbv_exit(void)
{
	tbv_ibdev_stop(&tbv_driver_state);
	tbv_services_stop(&tbv_driver_state);
	tbv_dmabuf_stop();
	tbv_core_exit(&tbv_driver_state);
	tbv_path_exit_optional_symbols();
	pr_info("unloaded\n");
}

module_init(tbv_init);
module_exit(tbv_exit);

MODULE_AUTHOR("thunderbolt-ibverbs contributors");
MODULE_DESCRIPTION("Thunderbolt/USB4 host-to-host RDMA verbs");
MODULE_LICENSE("GPL");
MODULE_VERSION(TBV_VERSION);
MODULE_SOFTDEP("pre: configfs ib_core ib_uverbs");
