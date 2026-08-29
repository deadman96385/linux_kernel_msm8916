/* SPDX-License-Identifier: GPL-2.0-only */
// Portable-source SHA-256: 17be86b48683fc1cf0365c14832b1e43018e02497f12f5bcd4cdfeb2fe0d65fd
#ifndef S6E8AA5X01_CURVE_H
#define S6E8AA5X01_CURVE_H

#include "s6e8aa5x01_gray.h"

#include <linux/types.h>

struct s6e8aa5x01_curve_desc {
	const s32 *target_coefficient;
	const s32 *search_curve;
	size_t count;
};

int s6e8aa5x01_curve_find(u16 *index, s64 target,
			  const s32 *curve, size_t count);

/*
 * Corrections are indexed by the clean voltage-point enum, not Samsung's
 * reversed V255..V3 table order. Output is modified only on complete success.
 */
int s6e8aa5x01_gamma_indices(u16 index[S6E8AA5X01_NUM_VOLTAGE_POINTS],
			     const struct s6e8aa5x01_curve_desc *desc, s32 base_luminance,
	const s16 correction[S6E8AA5X01_NUM_VOLTAGE_POINTS]);

#endif
