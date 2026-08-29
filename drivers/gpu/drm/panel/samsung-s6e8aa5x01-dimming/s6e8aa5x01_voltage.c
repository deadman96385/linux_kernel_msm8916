// SPDX-License-Identifier: GPL-2.0-only
// Portable-source SHA-256: 16ffa982869782e526d7b85c0eb148bd16ceeaed07d0553949bcbb3412dd7454
#include <linux/kernel.h>
#include "s6e8aa5x01_voltage.h"

#include <linux/errno.h>
#include <linux/limits.h>
#include <linux/types.h>

#define S6E8AA5X01_FIXED_SHIFT 22

const struct s6e8aa5x01_voltage_desc s6e8aa5x01_j5_j5x_voltage_desc = {
	.vregout = 24326963, /* 5.8 * 2^22, truncated exactly as stock. */
	.center = {
		[S6E8AA5X01_V3] = { 128, 128, 128 },
		[S6E8AA5X01_V11] = { 128, 128, 128 },
		[S6E8AA5X01_V23] = { 128, 128, 128 },
		[S6E8AA5X01_V35] = { 128, 128, 128 },
		[S6E8AA5X01_V51] = { 128, 128, 128 },
		[S6E8AA5X01_V87] = { 128, 128, 128 },
		[S6E8AA5X01_V151] = { 128, 128, 128 },
		[S6E8AA5X01_V203] = { 128, 128, 128 },
		[S6E8AA5X01_V255] = { 256, 256, 256 },
	},
};

static const u16 s6e8aa5x01_vt_coefficient[16] = {
	0, 12, 24, 36, 48, 60, 72, 84,
	96, 108, 138, 148, 158, 168, 178, 186,
};

static int s6e8aa5x01_between(s32 *result, s32 high, s32 low,
			      unsigned int factor, unsigned int denominator)
{
	u64 fixed_ratio;
	u64 drop;

	if (!result || high <= low || !factor || factor >= denominator)
		return -EDOM;

	fixed_ratio = ((u64)factor << S6E8AA5X01_FIXED_SHIFT) /
		      denominator;
	drop = ((u64)(high - low) * fixed_ratio) >>
	       S6E8AA5X01_FIXED_SHIFT;
	if (!drop || drop >= (u64)(high - low))
		return -EDOM;

	*result = high - (s32)drop;
	return 0;
}

static int s6e8aa5x01_validate_input(const struct s6e8aa5x01_voltage_desc *desc,
				     const struct s6e8aa5x01_mtp *mtp)
{
	size_t point;
	size_t color;

	if (!desc || !mtp || desc->vregout <= 0)
		return -EINVAL;

	for (color = 0; color < S6E8AA5X01_NUM_COLORS; color++) {
		if (mtp->vt_index[color] >=
		    sizeof(s6e8aa5x01_vt_coefficient) /
		    sizeof(s6e8aa5x01_vt_coefficient[0]))
			return -ERANGE;
		if (mtp->offset[S6E8AA5X01_V255][color] < -255 ||
		    mtp->offset[S6E8AA5X01_V255][color] > 255)
			return -ERANGE;
	}

	for (point = S6E8AA5X01_V3; point < S6E8AA5X01_V255;
	     point++)
		for (color = 0; color < S6E8AA5X01_NUM_COLORS; color++)
			if (mtp->offset[point][color] < -127 ||
			    mtp->offset[point][color] > 127)
				return -ERANGE;

	return 0;
}

int s6e8aa5x01_voltage_init(struct s6e8aa5x01_voltages *voltages,
			    const struct s6e8aa5x01_voltage_desc *desc,
				  const struct s6e8aa5x01_mtp *mtp)
{
	struct s6e8aa5x01_voltages calculated = { 0 };
	static const enum s6e8aa5x01_voltage_point descending[] = {
		S6E8AA5X01_V203,
		S6E8AA5X01_V151,
		S6E8AA5X01_V87,
		S6E8AA5X01_V51,
		S6E8AA5X01_V35,
		S6E8AA5X01_V23,
		S6E8AA5X01_V11,
	};
	size_t color;
	size_t i;
	int ret;

	if (!voltages)
		return -EINVAL;

	ret = s6e8aa5x01_validate_input(desc, mtp);
	if (ret)
		return ret;

	calculated.vregout = desc->vregout;
	for (color = 0; color < S6E8AA5X01_NUM_COLORS; color++) {
		int adjusted;
		s32 previous;

		adjusted = desc->center[S6E8AA5X01_V255][color] +
			   mtp->offset[S6E8AA5X01_V255][color];
		ret = s6e8aa5x01_between(&calculated.anchor[S6E8AA5X01_V255][color],
					 desc->vregout, 0, 72 + adjusted, 860);
		if (ret)
			return ret;

		ret = s6e8aa5x01_between(&calculated.vt[color], desc->vregout, 0,
					 s6e8aa5x01_vt_coefficient[mtp->vt_index[color]],
			860);
		if (ret) {
			/* VT index zero intentionally means exactly VREG. */
			if (mtp->vt_index[color])
				return ret;
			calculated.vt[color] = desc->vregout;
		}

		if (calculated.vt[color] <=
		    calculated.anchor[S6E8AA5X01_V255][color])
			return -EDOM;

		previous = calculated.anchor[S6E8AA5X01_V255][color];
		for (i = 0; i < ARRAY_SIZE(descending);
		     i++) {
			enum s6e8aa5x01_voltage_point point = descending[i];

			adjusted = desc->center[point][color] +
				   mtp->offset[point][color];
			ret = s6e8aa5x01_between(&calculated.anchor[point][color],
						 calculated.vt[color], previous,
				64 + adjusted, 320);
			if (ret)
				return ret;
			previous = calculated.anchor[point][color];
		}

		adjusted = desc->center[S6E8AA5X01_V3][color] +
			   mtp->offset[S6E8AA5X01_V3][color];
		ret = s6e8aa5x01_between(&calculated.anchor[S6E8AA5X01_V3][color],
					 desc->vregout,
			calculated.anchor[S6E8AA5X01_V11][color],
			64 + adjusted, 320);
		if (ret)
			return ret;
	}

	*voltages = calculated;
	return 0;
}
