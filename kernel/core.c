// SPDX-License-Identifier: GPL-2.0

#define pr_fmt(fmt) "thunderbolt_ibverbs: " fmt

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/random.h>
#include <linux/string.h>
#include <linux/workqueue.h>

#include "tbv.h"

int tbv_core_init(struct tbv_state *state, const struct tbv_config *cfg)
{
	int ret;

	memset(state, 0, sizeof(*state));
	state->cfg = *cfg;
	mutex_init(&state->lock);
	mutex_init(&state->rail_register_lock);
	INIT_LIST_HEAD(&state->peers);
	INIT_LIST_HEAD(&state->configured_links);
	xa_init(&state->verbs_mrs_xa);
	xa_init(&state->verbs_qps_xa);
	state->next_peer_id = 1;
	get_random_bytes(state->native_host_id, sizeof(state->native_host_id));
	state->workqueue = alloc_workqueue("tbv_ibdev",
					   WQ_UNBOUND |
						   (tbv_dmabuf_verbs_enabled() ? 0 : WQ_MEM_RECLAIM) |
						   WQ_HIGHPRI,
					   0);
	if (!state->workqueue) {
		mutex_destroy(&state->rail_register_lock);
		mutex_destroy(&state->lock);
		return -ENOMEM;
	}

	ret = tbv_debugfs_init(state);
	if (ret)
		goto err_destroy_wq;

	ret = tbv_configfs_start(state);
	if (ret) {
		tbv_debugfs_exit(state);
		goto err_destroy_wq;
	}

	pr_info("core ready rc=1 uc=1\n");
	return 0;

err_destroy_wq:
	destroy_workqueue(state->workqueue);
	state->workqueue = NULL;
	mutex_destroy(&state->rail_register_lock);
	mutex_destroy(&state->lock);
	return ret;
}

void tbv_core_exit(struct tbv_state *state)
{
	u32 configured_link_count;

	if (!list_empty(&state->peers))
		pr_warn("unloading with live peers after service teardown\n");

	tbv_configfs_stop(state);
	configured_link_count = tbv_link_count(state);
	if (configured_link_count)
		pr_warn("unloading with %u configured links after configfs teardown\n",
			configured_link_count);

	if (state->workqueue) {
		flush_workqueue(state->workqueue);
		destroy_workqueue(state->workqueue);
		state->workqueue = NULL;
	}

	tbv_debugfs_exit(state);
	if (!xa_empty(&state->verbs_mrs_xa))
		pr_warn("unloading with live MR registry entries\n");
	if (!xa_empty(&state->verbs_qps_xa))
		pr_warn("unloading with live QP registry entries\n");
	xa_destroy(&state->verbs_mrs_xa);
	xa_destroy(&state->verbs_qps_xa);
	mutex_destroy(&state->rail_register_lock);
	mutex_destroy(&state->lock);
}
