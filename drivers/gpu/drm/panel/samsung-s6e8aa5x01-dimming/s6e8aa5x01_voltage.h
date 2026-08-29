/* SPDX-License-Identifier: GPL-2.0-only */
// Portable-source SHA-256: 73dc029bcd742288de85b33f2ee61bdab568847e7f4accca9ba411e600fcf7d6
#ifndef S6E8AA5X01_VOLTAGE_H
#define S6E8AA5X01_VOLTAGE_H

#include "s6e8aa5x01_mtp.h"

#include <linux/types.h>

struct s6e8aa5x01_voltage_desc {
	s32 vregout;
	u16 center[S6E8AA5X01_NUM_VOLTAGE_POINTS]
		       [S6E8AA5X01_NUM_COLORS];
	u8 vt_center[S6E8AA5X01_NUM_COLORS];
};

struct s6e8aa5x01_voltages {
	s32 vregout;
	s32 vt[S6E8AA5X01_NUM_COLORS];
	s32 anchor[S6E8AA5X01_NUM_VOLTAGE_POINTS]
		      [S6E8AA5X01_NUM_COLORS];
};

/* Shared factory center values in both audited J5/J5x stock sources. */
extern const struct s6e8aa5x01_voltage_desc
	s6e8aa5x01_j5_j5x_voltage_desc;

/* The output is modified only after the complete physical state is valid. */
int s6e8aa5x01_voltage_init(struct s6e8aa5x01_voltages *voltages,
			    const struct s6e8aa5x01_voltage_desc *desc,
				  const struct s6e8aa5x01_mtp *mtp);

#endif
