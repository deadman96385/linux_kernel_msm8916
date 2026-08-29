/* SPDX-License-Identifier: GPL-2.0-only */
// Portable-source SHA-256: 5b227f9f74956af76702db51e70a643c8eb124c758e3274f52a8c6c45b5736ea
#ifndef S6E8AA5X01_UPDATE_H
#define S6E8AA5X01_UPDATE_H

#include "s6e8aa5x01_dimming.h"
#include "s6e8aa5x01_policy.h"

#include <linux/types.h>

#define S6E8AA5X01_GAMMA_COMMAND_LEN (S6E8AA5X01_GAMMA_LEN + 1)
#define S6E8AA5X01_NUM_NORMAL_UPDATE_COMMANDS 10

typedef int (*s6e8aa5x01_write_fn)(void *context, const u8 *data,
				   size_t length);

struct s6e8aa5x01_variant {
	const char *name;
	const struct s6e8aa5x01_dimming_desc *dimming;
	const struct s6e8aa5x01_panel_policy *policy;
};

struct s6e8aa5x01_normal_update {
	bool valid;
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
				   unsigned int brightness, bool acl_enabled,
				   int temperature, bool factory_elvss_valid,
				   u8 factory_elvss);

/* Emits the complete body in stock order and stops at the first write error. */
int s6e8aa5x01_normal_update_emit(const struct s6e8aa5x01_normal_update *update,
				  s6e8aa5x01_write_fn write, void *context);

#endif
