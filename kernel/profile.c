// SPDX-License-Identifier: GPL-2.0

#define pr_fmt(fmt) "thunderbolt_ibverbs: " fmt

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>

#include "tbv.h"

static int tbv_parse_lanes(struct tbv_config *cfg, const char *input)
{
	unsigned int min;
	unsigned int max;
	int ret;

	if (!input || !*input || !strcmp(input, "auto")) {
		cfg->lanes_auto = true;
		cfg->lanes_min = 0;
		cfg->lanes_max = 0;
		return 0;
	}

	ret = sscanf(input, "%u-%u", &min, &max);
	if (ret == 1) {
		max = min;
	} else if (ret != 2) {
		pr_err("invalid lanes='%s'\n", input);
		return -EINVAL;
	}

	if (!min || max < min) {
		pr_err("invalid lanes range '%s'\n", input);
		return -EINVAL;
	}

	cfg->lanes_auto = false;
	cfg->lanes_min = min;
	cfg->lanes_max = max;
	return 0;
}

static int tbv_parse_deprecated(const char *param, const char *input,
				const char * const *accepted,
				const char * const *removed)
{
	int i;

	if (!input || !*input) {
		pr_err("invalid %s=''\n", param);
		return -EINVAL;
	}
	if (!strcmp(input, "auto"))
		return 0;

	for (i = 0; removed[i]; i++) {
		if (!strcmp(input, removed[i])) {
			pr_err("%s=%s is no longer supported: Apple transport support was removed\n",
			       param, input);
			return -EINVAL;
		}
	}
	for (i = 0; accepted[i]; i++) {
		if (!strcmp(input, accepted[i])) {
			pr_warn("%s=%s is deprecated and ignored\n", param, input);
			return 0;
		}
	}

	pr_err("invalid %s='%s'\n", param, input);
	return -EINVAL;
}

int tbv_config_parse(struct tbv_config *cfg, const char *compat,
		     const char *profile, const char *tbnet, const char *lanes)
{
	static const char * const compat_ok[] = { "off", NULL };
	static const char * const compat_removed[] = { "force", NULL };
	static const char * const profile_ok[] = { "linux_perf", "mixed", NULL };
	static const char * const profile_removed[] = { "mac_compat", NULL };
	static const char * const tbnet_ok[] = {
		"allow", "prefer_rdma", "block", NULL
	};
	static const char * const tbnet_removed[] = { NULL };
	int ret;

	memset(cfg, 0, sizeof(*cfg));

	ret = tbv_parse_deprecated("compat", compat, compat_ok, compat_removed);
	ret = ret ?: tbv_parse_deprecated("profile", profile, profile_ok,
					  profile_removed);
	ret = ret ?: tbv_parse_deprecated("tbnet", tbnet, tbnet_ok,
					  tbnet_removed);
	if (ret)
		return ret;

	return tbv_parse_lanes(cfg, lanes);
}
