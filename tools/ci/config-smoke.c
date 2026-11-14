// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
#include <stdio.h>

#include "proto/config.h"

#define CHECK(cond)                                                            \
	do {                                                                   \
		if (!(cond)) {                                                 \
			fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, \
				__LINE__, #cond);                               \
			return 1;                                              \
		}                                                              \
	} while (0)

static struct tbv_id_gid mk_gid(tbv_id_u32 dev, tbv_id_u8 port,
				tbv_id_u8 index, tbv_id_u8 type,
				tbv_id_u8 a, tbv_id_u8 b, tbv_id_u8 c,
				tbv_id_u8 d)
{
	struct tbv_id_gid gid = {
		.device_id = dev,
		.port = port,
		.gid_index = index,
		.gid_type = type,
	};

	tbv_id_addr_v4(&gid.addr, a, b, c, d);
	return gid;
}

static struct tbv_id_route mk_route(tbv_id_u8 peer_a, tbv_id_u8 peer_b,
				    tbv_id_u8 peer_c, tbv_id_u8 peer_d,
				    tbv_id_u8 src_a, tbv_id_u8 src_b,
				    tbv_id_u8 src_c, tbv_id_u8 src_d)
{
	struct tbv_id_route route;

	tbv_id_addr_v4(&route.peer_addr, peer_a, peer_b, peer_c, peer_d);
	tbv_id_addr_v4(&route.local_src_addr, src_a, src_b, src_c, src_d);
	return route;
}

static int configure_canonical(struct tbv_cfg_link *link, tbv_id_u8 backend)
{
	struct tbv_id_gid gids[] = {
		mk_gid(4, 1, 1, TBV_ID_GID_ROCE_V2, 10, 0, 4, 2),
	};
	struct tbv_id_route route = mk_route(10, 0, 5, 2, 10, 0, 4, 2);

	CHECK(tbv_cfg_link_set_backend(link, backend) == 0);
	CHECK(tbv_cfg_link_set_route(link, &route) == 0);
	CHECK(tbv_cfg_link_set_app_gids(link, gids, 1) == 0);

	return 0;
}

static int test_canonical_native_activates(void)
{
	struct tbv_cfg_link link;

	tbv_cfg_link_init(&link, 1);
	CHECK(configure_canonical(&link, TBV_CFG_BACKEND_NATIVE) == 0);
	CHECK(tbv_cfg_link_seal(&link) == 0);
	CHECK(link.state == TBV_CFG_SEALED);
	CHECK(link.app_selection.device_id == 4);
	CHECK(tbv_cfg_link_activate(&link) == 0);
	CHECK(link.state == TBV_CFG_ACTIVE);

	return 0;
}

static int test_only_native_backend_accepted(void)
{
	struct tbv_cfg_link link;

	tbv_cfg_link_init(&link, 2);
	CHECK(tbv_cfg_link_set_backend(&link, TBV_CFG_BACKEND_NONE) == -EINVAL);
	CHECK(tbv_cfg_link_set_backend(&link, 2) == -EINVAL);
	CHECK(link.backend == TBV_CFG_BACKEND_NONE);

	return 0;
}

static int test_partial_links_cannot_activate(void)
{
	struct tbv_cfg_link link;
	struct tbv_id_route route = mk_route(10, 0, 5, 2, 10, 0, 4, 2);

	tbv_cfg_link_init(&link, 3);
	CHECK(tbv_cfg_link_activate(&link) == -EINVAL);
	CHECK(tbv_cfg_link_set_backend(&link, TBV_CFG_BACKEND_NATIVE) == 0);
	CHECK(tbv_cfg_link_seal(&link) == -EINVAL);
	CHECK(tbv_cfg_link_set_route(&link, &route) == 0);
	CHECK(tbv_cfg_link_set_app_gids(&link, NULL, 1) == -EINVAL);
	CHECK(tbv_cfg_link_seal(&link) == -EINVAL);

	return 0;
}

static int test_current_bridge_shape_cannot_seal(void)
{
	struct tbv_id_gid gids[] = {
		mk_gid(4, 1, 1, TBV_ID_GID_ROCE_V2, 10, 0, 4, 2),
		mk_gid(5, 1, 1, TBV_ID_GID_ROCE_V2, 192, 168, 23, 136),
		mk_gid(6, 1, 1, TBV_ID_GID_ROCE_V2, 192, 168, 23, 136),
	};
	struct tbv_id_route route =
		mk_route(192, 168, 23, 192, 192, 168, 23, 136);
	struct tbv_cfg_link link;

	tbv_cfg_link_init(&link, 4);
	CHECK(tbv_cfg_link_set_backend(&link, TBV_CFG_BACKEND_NATIVE) == 0);
	CHECK(tbv_cfg_link_set_route(&link, &route) == 0);
	CHECK(tbv_cfg_link_set_app_gids(&link, gids, 3) == 0);
	CHECK(tbv_cfg_link_seal(&link) == -EEXIST);
	CHECK(link.state == TBV_CFG_DRAFT);

	return 0;
}

static int test_nccl_workaround_mismatch_cannot_seal(void)
{
	struct tbv_id_gid gids[] = {
		mk_gid(4, 1, 1, TBV_ID_GID_ROCE_V2, 10, 0, 4, 2),
		mk_gid(5, 1, 1, TBV_ID_GID_ROCE_V2, 192, 168, 23, 136),
	};
	struct tbv_id_route route = mk_route(10, 0, 5, 2, 192, 168, 23, 136);
	struct tbv_id_nccl_policy nccl;
	struct tbv_cfg_link link;

	tbv_id_nccl_policy_default(&nccl);
	tbv_id_addr_v4(&nccl.addr_range, 10, 0, 0, 0);
	nccl.has_addr_range = true;
	nccl.addr_range_bits = 8;

	tbv_cfg_link_init(&link, 5);
	CHECK(tbv_cfg_link_set_backend(&link, TBV_CFG_BACKEND_NATIVE) == 0);
	CHECK(tbv_cfg_link_set_route(&link, &route) == 0);
	CHECK(tbv_cfg_link_set_nccl_policy(&link, &nccl) == 0);
	CHECK(tbv_cfg_link_set_app_gids(&link, gids, 2) == 0);
	CHECK(tbv_cfg_link_seal(&link) == -EXDEV);

	return 0;
}

static int test_sealed_links_are_immutable(void)
{
	struct tbv_cfg_link link;
	struct tbv_id_route route = mk_route(10, 0, 5, 2, 10, 0, 4, 2);

	tbv_cfg_link_init(&link, 6);
	CHECK(configure_canonical(&link, TBV_CFG_BACKEND_NATIVE) == 0);
	CHECK(tbv_cfg_link_seal(&link) == 0);
	CHECK(tbv_cfg_link_set_route(&link, &route) == -EBUSY);
	CHECK(tbv_cfg_link_set_backend(&link, TBV_CFG_BACKEND_NATIVE) == -EBUSY);

	return 0;
}

static int test_external_names_are_canonical(void)
{
	CHECK(tbv_cfg_link_validate_name("usb4_rdma0") == 0);
	CHECK(tbv_cfg_link_validate_name("usb4_rdma_strix") == 0);
	CHECK(tbv_cfg_link_validate_name("vm-link") == -EINVAL);
	CHECK(tbv_cfg_link_validate_name("usb4_apple0") == -EINVAL);
	CHECK(tbv_cfg_link_validate_name("usb4_rdma") == -EINVAL);
	CHECK(tbv_cfg_link_validate_name("usb4_rdma_abcdef") == -ENAMETOOLONG);

	return 0;
}

static int publish_calls;
static int publish_result;
static int publish_saw_state;

static int record_publish(void *ctx)
{
	struct tbv_cfg_link *link = ctx;

	publish_calls++;
	publish_saw_state = link->state;
	return publish_result;
}

/*
 * configfs activation used to be checked by extracting activate_store() out
 * of kernel/configfs.c and compiling it against a shim. The ordering it cared
 * about lives here instead, so this links the real object.
 */
static int test_publication_orders_and_rolls_back(void)
{
	struct tbv_cfg_link link;
	struct tbv_cfg_link fresh;

	tbv_cfg_link_init(&link, 8);
	CHECK(configure_canonical(&link, TBV_CFG_BACKEND_NATIVE) == 0);
	CHECK(tbv_cfg_link_seal(&link) == 0);

	/*
	 * Publication sees an active link, and a failure rolls it back to
	 * sealed rather than leaving it half published.
	 */
	publish_calls = 0;
	publish_result = -ENOMEM;
	publish_saw_state = -1;
	CHECK(tbv_cfg_link_activate_published(&link, record_publish,
					      &link) == -ENOMEM);
	CHECK(publish_calls == 1);
	CHECK(publish_saw_state == TBV_CFG_ACTIVE);
	CHECK(link.state == TBV_CFG_SEALED);

	/* The rollback left it activatable again. */
	publish_calls = 0;
	publish_result = 0;
	CHECK(tbv_cfg_link_activate_published(&link, record_publish,
					      &link) == 0);
	CHECK(publish_calls == 1);
	CHECK(link.state == TBV_CFG_ACTIVE);

	/* Activating twice is refused and does not publish again. */
	publish_calls = 0;
	CHECK(tbv_cfg_link_activate_published(&link, record_publish,
					      &link) == -EINVAL);
	CHECK(publish_calls == 0);
	CHECK(link.state == TBV_CFG_ACTIVE);

	/* A link that was never sealed is refused before publication. */
	tbv_cfg_link_init(&fresh, 9);
	publish_calls = 0;
	publish_result = 0;
	CHECK(tbv_cfg_link_activate_published(&fresh, record_publish,
					      &fresh) == -EINVAL);
	CHECK(publish_calls == 0);
	CHECK(fresh.state == TBV_CFG_EMPTY);

	return 0;
}

static int unpublish_calls;
static int unpublish_saw_state;

static void record_unpublish(void *ctx)
{
	struct tbv_cfg_link *link = ctx;

	unpublish_calls++;
	unpublish_saw_state = link->state;
}

static int test_release_unpublishes_only_active(void)
{
	struct tbv_cfg_link link;
	struct tbv_cfg_link fresh;

	tbv_cfg_link_init(&link, 10);
	CHECK(configure_canonical(&link, TBV_CFG_BACKEND_NATIVE) == 0);
	CHECK(tbv_cfg_link_seal(&link) == 0);
	publish_result = 0;
	CHECK(tbv_cfg_link_activate_published(&link, record_publish,
					      &link) == 0);

	unpublish_calls = 0;
	unpublish_saw_state = -1;
	tbv_cfg_link_release(&link, record_unpublish, &link);
	CHECK(unpublish_calls == 1);
	CHECK(unpublish_saw_state == TBV_CFG_ACTIVE);
	CHECK(link.state == TBV_CFG_SEALED);

	unpublish_calls = 0;
	tbv_cfg_link_release(&link, record_unpublish, &link);
	CHECK(unpublish_calls == 0);
	CHECK(link.state == TBV_CFG_SEALED);

	tbv_cfg_link_init(&fresh, 11);
	tbv_cfg_link_release(&fresh, record_unpublish, &fresh);
	CHECK(unpublish_calls == 0);
	CHECK(fresh.state == TBV_CFG_EMPTY);

	return 0;
}

int main(void)
{
	CHECK(test_canonical_native_activates() == 0);
	CHECK(test_only_native_backend_accepted() == 0);
	CHECK(test_partial_links_cannot_activate() == 0);
	CHECK(test_current_bridge_shape_cannot_seal() == 0);
	CHECK(test_nccl_workaround_mismatch_cannot_seal() == 0);
	CHECK(test_sealed_links_are_immutable() == 0);
	CHECK(test_external_names_are_canonical() == 0);
	CHECK(test_publication_orders_and_rolls_back() == 0);
	CHECK(test_release_unpublishes_only_active() == 0);

	puts("config smoke OK");
	return 0;
}
