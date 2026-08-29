/* SPDX-License-Identifier: GPL-2.0-only */
// Portable-source SHA-256: f6efc548f1cf28b6bb2acae339558dcfe3cedfdd36a27f31dc73941dc885a996
#ifndef S6E8AA5X01_UPDATE_H
#define S6E8AA5X01_UPDATE_H

#include "s6e8aa5x01_dimming.h"
#include "s6e8aa5x01_policy.h"

#include <linux/types.h>

#define S6E8AA5X01_GAMMA_COMMAND_LEN (S6E8AA5X01_GAMMA_LEN + 1)

struct s6e8aa5x01_variant {
	const char *name;
	const struct s6e8aa5x01_dimming_desc *dimming;
	const struct s6e8aa5x01_panel_policy *policy;
};

struct s6e8aa5x01_normal_update {
	u8 level;
	bool acl_enabled;
	bool uses_factory_elvss;
	u8 aid[S6E8AA5X01_PANEL_COMMAND_LEN];
	u8 acl[S6E8AA5X01_NUM_POLICY_COMMANDS]
		   [S6E8AA5X01_POLICY_COMMAND_LEN];
	u8 elvss[S6E8AA5X01_PANEL_COMMAND_LEN];
	u8 temperature[S6E8AA5X01_NUM_POLICY_COMMANDS]
			   [S6E8AA5X01_POLICY_COMMAND_LEN];
	u8 temperature_elvss[S6E8AA5X01_NUM_POLICY_COMMANDS]
				 [S6E8AA5X01_POLICY_COMMAND_LEN];
	u8 gamma[S6E8AA5X01_GAMMA_COMMAND_LEN];
	u8 gamma_latch[S6E8AA5X01_POLICY_COMMAND_LEN];
};

extern const struct s6e8aa5x01_variant s6e8aa5x01_j5_a_variant;
extern const struct s6e8aa5x01_variant s6e8aa5x01_j5_c_variant;
extern const struct s6e8aa5x01_variant s6e8aa5x01_j5x_variant;

/* Output is modified only after the complete update has been resolved. */
int s6e8aa5x01_normal_update_build(struct s6e8aa5x01_normal_update *result,
				   const struct s6e8aa5x01_variant *variant,
	const struct s6e8aa5x01_dimming *dimming,
	unsigned int brightness, bool acl_enabled, int temperature,
	bool factory_elvss_valid, u8 factory_elvss);

#endif
