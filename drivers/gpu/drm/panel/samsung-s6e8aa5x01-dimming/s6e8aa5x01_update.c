// SPDX-License-Identifier: GPL-2.0-only
// Portable-source SHA-256: c122a3f3493851681f365ec9b99897b9084754aac836a3cc85c76fa972e871ed
#include "s6e8aa5x01_update.h"

#include <linux/errno.h>
#include <linux/string.h>

const struct s6e8aa5x01_variant s6e8aa5x01_j5_a_variant = {
	.name = "AMS497HY01 revision A",
	.dimming = &s6e8aa5x01_j5_a_desc,
	.policy = &s6e8aa5x01_j5_a_policy,
};

const struct s6e8aa5x01_variant s6e8aa5x01_j5_c_variant = {
	.name = "AMS497HY01 revision C",
	.dimming = &s6e8aa5x01_j5_c_desc,
	.policy = &s6e8aa5x01_j5_c_policy,
};

const struct s6e8aa5x01_variant s6e8aa5x01_j5x_variant = {
	.name = "AMS520KT01",
	.dimming = &s6e8aa5x01_j5x_desc,
	.policy = &s6e8aa5x01_j5x_policy,
};

static int s6e8aa5x01_variant_validate(const struct s6e8aa5x01_variant *variant)
{
	if (!variant || !variant->name || !variant->dimming || !variant->policy)
		return -EINVAL;

	return s6e8aa5x01_policy_validate(variant->policy);
}

int s6e8aa5x01_normal_update_build(struct s6e8aa5x01_normal_update *result,
				   const struct s6e8aa5x01_variant *variant,
	const struct s6e8aa5x01_dimming *dimming,
	unsigned int brightness, bool acl_enabled, int temperature,
	bool factory_elvss_valid, u8 factory_elvss)
{
	struct s6e8aa5x01_normal_update calculated = { 0 };
	struct s6e8aa5x01_temperature_result temperature_result;
	const u8 *gamma;
	int ret;

	if (!result || !dimming)
		return -EINVAL;
	ret = s6e8aa5x01_variant_validate(variant);
	if (ret)
		return ret;
	if (!dimming->valid || dimming->desc != variant->dimming)
		return -EINVAL;

	ret = s6e8aa5x01_policy_level(variant->policy, brightness,
				      &calculated.level);
	if (ret)
		return ret;
	gamma = s6e8aa5x01_dimming_gamma(dimming, calculated.level);
	if (!gamma)
		return -EINVAL;
	ret = s6e8aa5x01_temperature_resolve(&temperature_result, variant->policy, calculated.level,
					     temperature, factory_elvss_valid, factory_elvss);
	if (ret)
		return ret;

	calculated.acl_enabled = acl_enabled;
	calculated.uses_factory_elvss =
		temperature_result.uses_factory_elvss;
	memcpy(calculated.aid, variant->policy->aid[calculated.level],
	       sizeof(calculated.aid));
	memcpy(calculated.acl,
	       acl_enabled ? variant->policy->acl_on : variant->policy->acl_off,
	       sizeof(calculated.acl));
	memcpy(calculated.elvss, variant->policy->elvss[calculated.level],
	       sizeof(calculated.elvss));
	memcpy(calculated.temperature, variant->policy->temperature_set,
	       sizeof(calculated.temperature));
	calculated.temperature[1][1] = temperature_result.encoded_temperature;
	memcpy(calculated.temperature_elvss,
	       variant->policy->temperature_elvss_set,
	       sizeof(calculated.temperature_elvss));
	calculated.temperature_elvss[1][1] = temperature_result.elvss;
	calculated.gamma[0] = 0xca;
	memcpy(&calculated.gamma[1], gamma, S6E8AA5X01_GAMMA_LEN);
	calculated.gamma_latch[0] = 0xf7;
	calculated.gamma_latch[1] = 0x03;

	*result = calculated;
	return 0;
}
