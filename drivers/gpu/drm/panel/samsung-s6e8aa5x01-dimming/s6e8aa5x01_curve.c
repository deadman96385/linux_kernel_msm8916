// SPDX-License-Identifier: GPL-2.0-only
// Portable-source SHA-256: 77da76d0d90e8a718ad431a42c997a3079d08b07ff02c8a3b180333511c52767
#include <linux/limits.h>
#include "s6e8aa5x01_curve.h"

#include <linux/errno.h>
#include <linux/types.h>

int s6e8aa5x01_curve_find(u16 *index, s64 target,
			  const s32 *curve, size_t count)
{
	size_t i;

	if (!index || !curve || count < 2 || count > U16_MAX || target < 0)
		return -EINVAL;

	for (i = 1; i < count; i++)
		if (curve[i] < curve[i - 1])
			return -EINVAL;

	if (target < curve[0] || target > curve[count - 1])
		return -ERANGE;

	for (i = 0; i < count - 1; i++) {
		s64 lower_delta = target - curve[i];
		s64 upper_delta = target - curve[i + 1];

		if (upper_delta < 0) {
			/* Match Samsung's lower-index result for an exact midpoint. */
			*index = lower_delta + upper_delta <= 0 ? i : i + 1;
			return 0;
		}
		if (!lower_delta) {
			*index = i;
			return 0;
		}
		if (!upper_delta) {
			*index = i + 1;
			return 0;
		}
	}

	/* target == curve[count - 1] is normally handled in the loop. */
	*index = count - 1;
	return 0;
}

int s6e8aa5x01_gamma_indices(u16 index[S6E8AA5X01_NUM_VOLTAGE_POINTS],
			     const struct s6e8aa5x01_curve_desc *desc, s32 base_luminance,
	const s16 correction[S6E8AA5X01_NUM_VOLTAGE_POINTS])
{
	u16 calculated[S6E8AA5X01_NUM_VOLTAGE_POINTS];
	static const unsigned int gray_point[S6E8AA5X01_NUM_VOLTAGE_POINTS] = {
		[S6E8AA5X01_V3] = 3,
		[S6E8AA5X01_V11] = 11,
		[S6E8AA5X01_V23] = 23,
		[S6E8AA5X01_V35] = 35,
		[S6E8AA5X01_V51] = 51,
		[S6E8AA5X01_V87] = 87,
		[S6E8AA5X01_V151] = 151,
		[S6E8AA5X01_V203] = 203,
		[S6E8AA5X01_V255] = 255,
	};
	size_t point;
	int ret;

	if (!index || !desc || !desc->target_coefficient ||
	    !desc->search_curve || !correction || base_luminance < 0 ||
	    desc->count < S6E8AA5X01_NUM_GRAY_LEVELS)
		return -EINVAL;

	for (point = 0; point < S6E8AA5X01_NUM_VOLTAGE_POINTS; point++) {
		s64 target =
			(s64)desc->target_coefficient[gray_point[point]] *
			base_luminance;
		int corrected;

		ret = s6e8aa5x01_curve_find(&calculated[point], target,
					    desc->search_curve, desc->count);
		if (ret)
			return ret;

		corrected = calculated[point] + correction[point];
		if (!corrected)
			corrected = 1;
		if (corrected < 0 ||
		    corrected >= S6E8AA5X01_NUM_GRAY_LEVELS)
			return -ERANGE;
		calculated[point] = corrected;

		if (point && calculated[point] <= calculated[point - 1])
			return -EDOM;
	}

	for (point = 0; point < S6E8AA5X01_NUM_VOLTAGE_POINTS; point++)
		index[point] = calculated[point];

	return 0;
}
