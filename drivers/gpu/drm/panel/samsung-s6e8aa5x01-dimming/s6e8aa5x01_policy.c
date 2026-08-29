// SPDX-License-Identifier: GPL-2.0-only
// Portable-source SHA-256: 3b2927b65ee5bdeb51f499725b55781ba591b75e4a4b2893089a39ce7d5023d4
#include "s6e8aa5x01_policy.h"

#include "s6e8aa5x01_tables.h"

#include <linux/errno.h>
#include <linux/types.h>

static int s6e8aa5x01_command_pair_validate(const u8 command[S6E8AA5X01_NUM_POLICY_COMMANDS]
			     [S6E8AA5X01_POLICY_COMMAND_LEN],
	u8 first, u8 second)
{
	if (!command || command[0][0] != first || command[1][0] != second)
		return -EINVAL;

	return 0;
}

static int s6e8aa5x01_acl_validate(const u8 command[S6E8AA5X01_NUM_POLICY_COMMANDS]
			     [S6E8AA5X01_POLICY_COMMAND_LEN], bool enable)
{
	size_t i;
	bool found_b5 = false;
	bool found_55 = false;

	for (i = 0; i < S6E8AA5X01_NUM_POLICY_COMMANDS; i++) {
		switch (command[i][0]) {
		case 0x55:
			if (found_55 || command[i][1] != (enable ? 0x02 : 0x00))
				return -EINVAL;
			found_55 = true;
			break;
		case 0xb5:
			if (found_b5 || command[i][1] != (enable ? 0x50 : 0x40))
				return -EINVAL;
			found_b5 = true;
			break;
		default:
			return -EINVAL;
		}
	}

	return found_b5 && found_55 ? 0 : -EINVAL;
}

int s6e8aa5x01_policy_validate(const struct s6e8aa5x01_panel_policy *policy)
{
	size_t brightness;
	size_t level;
	int ret;

	if (!policy || !policy->name || !policy->brightness_to_level ||
	    !policy->aid || !policy->elvss || !policy->acl_on ||
	    !policy->acl_off || !policy->temperature_set ||
	    !policy->temperature_elvss_set || !policy->warm_elvss ||
	    !policy->cool_elvss || !policy->cold_elvss ||
	    policy->cold_threshold < -127 || policy->cold_threshold >= 0)
		return -EINVAL;

	for (brightness = 0;
	     brightness < S6E8AA5X01_NUM_BRIGHTNESS_LEVELS; brightness++) {
		if (policy->brightness_to_level[brightness] >=
		    S6E8AA5X01_NUM_NORMAL_LEVELS)
			return -ERANGE;
		if (brightness && policy->brightness_to_level[brightness] <
				  policy->brightness_to_level[brightness - 1])
			return -EINVAL;
	}

	for (level = 0; level < S6E8AA5X01_NUM_NORMAL_LEVELS; level++) {
		if (policy->aid[level][0] != 0xb2 ||
		    policy->elvss[level][0] != 0xb6)
			return -EINVAL;
		if (s6e8aa5x01_candela[level] <= 29) {
			if (policy->warm_elvss[level] == S6E8AA5X01_FACTORY_ELVSS ||
			    policy->cool_elvss[level] == S6E8AA5X01_FACTORY_ELVSS ||
			    policy->cold_elvss[level] == S6E8AA5X01_FACTORY_ELVSS)
				return -EINVAL;
		} else if (policy->warm_elvss[level] != S6E8AA5X01_FACTORY_ELVSS ||
			   policy->cool_elvss[level] != S6E8AA5X01_FACTORY_ELVSS ||
			   policy->cold_elvss[level] != S6E8AA5X01_FACTORY_ELVSS) {
			return -EINVAL;
		}
	}

	if (policy->acl_on[0][0] == 0x55)
		ret = s6e8aa5x01_command_pair_validate(policy->acl_on,
						       0x55, 0xb5);
	else
		ret = s6e8aa5x01_command_pair_validate(policy->acl_on,
						       0xb5, 0x55);
	if (ret)
		return ret;
	ret = s6e8aa5x01_acl_validate(policy->acl_on, true);
	if (ret)
		return ret;
	if (policy->acl_off[0][0] != policy->acl_on[0][0] ||
	    policy->acl_off[1][0] != policy->acl_on[1][0])
		return -EINVAL;
	ret = s6e8aa5x01_acl_validate(policy->acl_off, false);
	if (ret)
		return ret;
	ret = s6e8aa5x01_command_pair_validate(policy->temperature_set,
					       0xb0, 0xb8);
	if (ret)
		return ret;
	if (policy->temperature_set[0][1] != 0x07)
		return -EINVAL;
	ret = s6e8aa5x01_command_pair_validate(policy->temperature_elvss_set,
					       0xb0, 0xb6);
	if (ret)
		return ret;
	if (policy->temperature_elvss_set[0][1] != 0x15)
		return -EINVAL;

	return 0;
}

int s6e8aa5x01_policy_level(const struct s6e8aa5x01_panel_policy *policy,
			    unsigned int brightness, u8 *level)
{
	int ret;

	if (!level)
		return -EINVAL;
	if (brightness >= S6E8AA5X01_NUM_BRIGHTNESS_LEVELS)
		return -ERANGE;
	ret = s6e8aa5x01_policy_validate(policy);
	if (ret)
		return ret;

	*level = policy->brightness_to_level[brightness];
	return 0;
}

int s6e8aa5x01_temperature_resolve(struct s6e8aa5x01_temperature_result *result,
				   const struct s6e8aa5x01_panel_policy *policy,
	unsigned int level, int temperature,
	bool factory_elvss_valid, u8 factory_elvss)
{
	struct s6e8aa5x01_temperature_result calculated;
	const u8 *table;
	u8 elvss;
	int ret;

	if (!result)
		return -EINVAL;
	if (level >= S6E8AA5X01_NUM_NORMAL_LEVELS ||
	    temperature < -127 || temperature > 127)
		return -ERANGE;
	ret = s6e8aa5x01_policy_validate(policy);
	if (ret)
		return ret;

	if (temperature > 0)
		table = policy->warm_elvss;
	else if (temperature > policy->cold_threshold)
		table = policy->cool_elvss;
	else
		table = policy->cold_elvss;

	elvss = table[level];
	calculated.uses_factory_elvss =
		elvss == S6E8AA5X01_FACTORY_ELVSS;
	if (calculated.uses_factory_elvss) {
		if (!factory_elvss_valid)
			return -ENODATA;
		elvss = factory_elvss;
	}

	calculated.encoded_temperature = temperature < 0 ?
		(u8)(-temperature) | 0x80 : (u8)temperature;
	calculated.elvss = elvss;
	*result = calculated;

	return 0;
}
