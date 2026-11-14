// SPDX-License-Identifier: GPL-2.0

#include <linux/errno.h>

#include "tbv.h"

int tbv_native_validate_config(const struct tbv_cfg_link *link)
{
	if (link->backend != TBV_CFG_BACKEND_NATIVE)
		return -EINVAL;
	if (!link->app_selection.valid)
		return -EINVAL;
	if (link->app_selection.gid_type != TBV_ID_GID_ROCE_V2)
		return -EINVAL;

	return 0;
}
