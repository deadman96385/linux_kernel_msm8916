/* SPDX-License-Identifier: GPL-2.0-only */
// Portable-source SHA-256: 84930ae7d7213dce3cc7fb0628f82633c353971e75caf66d6cb43ada963fe1a0
#ifndef S6E8AA5X01_POLICY_H
#define S6E8AA5X01_POLICY_H

#include "s6e8aa5x01_panel_tables.h"

#include <linux/types.h>

struct s6e8aa5x01_temperature_result {
	u8 encoded_temperature;
	u8 elvss;
	bool uses_factory_elvss;
};

int s6e8aa5x01_policy_validate(const struct s6e8aa5x01_panel_policy *policy);

int s6e8aa5x01_policy_level(const struct s6e8aa5x01_panel_policy *policy,
			    unsigned int brightness, u8 *level);

/* Output is modified only after all inputs and factory-data needs are valid. */
int s6e8aa5x01_temperature_resolve(struct s6e8aa5x01_temperature_result *result,
				   const struct s6e8aa5x01_panel_policy *policy,
	unsigned int level, int temperature,
	bool factory_elvss_valid, u8 factory_elvss);

#endif
