/* SPDX-License-Identifier: GPL-2.0-only */
// Portable-source SHA-256: 6cd12fcac8a477aa9587252877f9d3338a357b37696da1df9f17e5008fb0802e
#ifndef S6E8AA5X01_GRAY_H
#define S6E8AA5X01_GRAY_H

#include "s6e8aa5x01_voltage.h"

#include <linux/types.h>

#define S6E8AA5X01_NUM_GRAY_LEVELS 256

struct s6e8aa5x01_gray_table {
	s32 value[S6E8AA5X01_NUM_GRAY_LEVELS]
		     [S6E8AA5X01_NUM_COLORS];
};

/* The output is modified only when every color has a valid monotonic table. */
int s6e8aa5x01_gray_init(struct s6e8aa5x01_gray_table *gray,
			 const struct s6e8aa5x01_voltages *voltages);

#endif
