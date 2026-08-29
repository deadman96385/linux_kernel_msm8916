// SPDX-License-Identifier: GPL-2.0-only
// Portable-source SHA-256: ff96644319bf8c4f813b27e5a7e8f83d0b2f8bfb7713d66224ffcaaf62b396c6
#include <linux/limits.h>
#include <linux/kernel.h>
#include "s6e8aa5x01_gamma.h"

#include <linux/errno.h>
#include <linux/types.h>

static int s6e8aa5x01_reverse_v255(u8 gamma[S6E8AA5X01_GAMMA_LEN],
				   const struct s6e8aa5x01_voltages *voltages,
	const struct s6e8aa5x01_gray_table *gray,
	const u16 index[S6E8AA5X01_NUM_VOLTAGE_POINTS])
{
	size_t color;

	for (color = 0; color < S6E8AA5X01_NUM_COLORS; color++) {
		s32 target = gray->value[index[S6E8AA5X01_V255]][color];
		s64 numerator;
		s64 code;
		u16 encoded;

		if (target < 0 || target > voltages->vregout)
			return -EDOM;

		numerator = (s64)voltages->vregout - target;
		code = numerator * 860 / voltages->vregout - 72;
		if (code < S16_MIN || code > U16_MAX)
			return -ERANGE;

		encoded = (u16)code;
		gamma[color * 2] = encoded >> 8;
		gamma[color * 2 + 1] = encoded;
	}

	return 0;
}

static int s6e8aa5x01_reverse_point(u8 gamma[S6E8AA5X01_GAMMA_LEN], unsigned int byte,
				    enum s6e8aa5x01_voltage_point point,
	const struct s6e8aa5x01_voltages *voltages,
	const struct s6e8aa5x01_gray_table *gray,
	const u16 index[S6E8AA5X01_NUM_VOLTAGE_POINTS])
{
	enum s6e8aa5x01_voltage_point higher = point + 1;
	size_t color;

	for (color = 0; color < S6E8AA5X01_NUM_COLORS; color++) {
		s32 reference = point == S6E8AA5X01_V3 ?
			voltages->vregout : voltages->vt[color];
		s32 target = gray->value[index[point]][color];
		s32 higher_target = gray->value[index[higher]][color];
		s64 numerator;
		s64 denominator;
		s64 code;

		if (target > reference || higher_target >= reference)
			return -EDOM;

		numerator = (s64)reference - target;
		denominator = (s64)reference - higher_target;
		if (denominator <= 0 || numerator < 0 ||
		    numerator > denominator)
			return -EDOM;

		code = numerator * 320 / denominator - 64;
		if (code < S8_MIN || code > U8_MAX)
			return -ERANGE;
		gamma[byte + color] = (u8)code;
	}

	return 0;
}

int s6e8aa5x01_gamma_reverse(u8 gamma[S6E8AA5X01_GAMMA_LEN],
			     const struct s6e8aa5x01_voltage_desc *desc,
	const struct s6e8aa5x01_voltages *voltages,
	const struct s6e8aa5x01_gray_table *gray,
	const u16 index[S6E8AA5X01_NUM_VOLTAGE_POINTS])
{
	u8 calculated[S6E8AA5X01_GAMMA_LEN] = { 0 };
	static const enum s6e8aa5x01_voltage_point descending[] = {
		S6E8AA5X01_V203,
		S6E8AA5X01_V151,
		S6E8AA5X01_V87,
		S6E8AA5X01_V51,
		S6E8AA5X01_V35,
		S6E8AA5X01_V23,
		S6E8AA5X01_V11,
		S6E8AA5X01_V3,
	};
	size_t point;
	size_t color;
	int ret;

	if (!gamma || !desc || !voltages || !gray || !index ||
	    voltages->vregout <= 0)
		return -EINVAL;

	for (point = 0; point < S6E8AA5X01_NUM_VOLTAGE_POINTS; point++) {
		if (index[point] >= S6E8AA5X01_NUM_GRAY_LEVELS)
			return -ERANGE;
		if (point && index[point] <= index[point - 1])
			return -EDOM;
	}

	ret = s6e8aa5x01_reverse_v255(calculated, voltages, gray, index);
	if (ret)
		return ret;

	for (point = 0; point < ARRAY_SIZE(descending);
	     point++) {
		ret = s6e8aa5x01_reverse_point(calculated, 6 + point * 3,
					       descending[point], voltages,
					       gray, index);
		if (ret)
			return ret;
	}

	for (color = 0; color < S6E8AA5X01_NUM_COLORS; color++)
		calculated[30 + color] = desc->vt_center[color];

	for (point = 0; point < S6E8AA5X01_GAMMA_LEN; point++)
		gamma[point] = calculated[point];

	return 0;
}

int s6e8aa5x01_gamma_compensate(u8 gamma[S6E8AA5X01_GAMMA_LEN],
				const u8 raw[S6E8AA5X01_GAMMA_LEN],
	const struct s6e8aa5x01_mtp *mtp,
	const s16 rgb[S6E8AA5X01_NUM_RGB_CORRECTIONS])
{
	u8 calculated[S6E8AA5X01_GAMMA_LEN];
	s32 work[S6E8AA5X01_GAMMA_LEN];
	static const enum s6e8aa5x01_voltage_point descending[] = {
		S6E8AA5X01_V203,
		S6E8AA5X01_V151,
		S6E8AA5X01_V87,
		S6E8AA5X01_V51,
		S6E8AA5X01_V35,
		S6E8AA5X01_V23,
		S6E8AA5X01_V11,
		S6E8AA5X01_V3,
	};
	size_t color;
	size_t i;

	if (!gamma || !raw || !mtp || !rgb)
		return -EINVAL;

	for (i = 0; i < S6E8AA5X01_GAMMA_LEN; i++)
		work[i] = raw[i];

	/* Stock applies the first three RGB corrections to 16-bit V255 codes. */
	for (color = 0; color < S6E8AA5X01_NUM_COLORS; color++) {
		u16 encoded = raw[color * 2] << 8 |
				   raw[color * 2 + 1];
		s32 adjusted = encoded + rgb[color];
		u16 wrapped = (u16)adjusted;
		s32 final = wrapped -
			mtp->offset[S6E8AA5X01_V255][color];

		if (final < 0 || final > 511)
			return -ERANGE;
		calculated[color * 2] = final >> 8;
		calculated[color * 2 + 1] = final;
	}

	/* V203..V11 have 21 byte-wise RGB correction entries. */
	for (i = 3; i < S6E8AA5X01_NUM_RGB_CORRECTIONS; i++)
		work[i + 3] += rgb[i];

	/* All one-byte V203..V3 results use stock's explicit saturation. */
	for (i = 0; i < ARRAY_SIZE(descending); i++) {
		enum s6e8aa5x01_voltage_point point = descending[i];

		for (color = 0; color < S6E8AA5X01_NUM_COLORS; color++) {
			s32 final = work[6 + i * 3 + color] -
				mtp->offset[point][color];

			if (final < 0)
				final = 0;
			else if (final > 255)
				final = 255;
			calculated[6 + i * 3 + color] = final;
		}
	}

	for (color = 0; color < S6E8AA5X01_NUM_COLORS; color++) {
		if (raw[30 + color] > 15)
			return -ERANGE;
		calculated[30 + color] = raw[30 + color];
	}

	for (i = 0; i < S6E8AA5X01_GAMMA_LEN; i++)
		gamma[i] = calculated[i];

	return 0;
}

int s6e8aa5x01_gamma_center(u8 gamma[S6E8AA5X01_GAMMA_LEN],
			    const struct s6e8aa5x01_voltage_desc *desc)
{
	u8 calculated[S6E8AA5X01_GAMMA_LEN] = { 0 };
	static const enum s6e8aa5x01_voltage_point descending[] = {
		S6E8AA5X01_V203,
		S6E8AA5X01_V151,
		S6E8AA5X01_V87,
		S6E8AA5X01_V51,
		S6E8AA5X01_V35,
		S6E8AA5X01_V23,
		S6E8AA5X01_V11,
		S6E8AA5X01_V3,
	};
	size_t color;
	size_t point;

	if (!gamma || !desc)
		return -EINVAL;

	for (color = 0; color < S6E8AA5X01_NUM_COLORS; color++) {
		u16 v255 = desc->center[S6E8AA5X01_V255][color];

		if (v255 > 511 || desc->vt_center[color] > 15)
			return -ERANGE;
		calculated[color * 2] = v255 >> 8;
		calculated[color * 2 + 1] = v255;
		calculated[30 + color] = desc->vt_center[color];
	}

	for (point = 0; point < ARRAY_SIZE(descending);
	     point++)
		for (color = 0; color < S6E8AA5X01_NUM_COLORS; color++) {
			u16 value = desc->center[descending[point]][color];

			if (value > 255)
				return -ERANGE;
			calculated[6 + point * 3 + color] = value;
		}

	for (point = 0; point < S6E8AA5X01_GAMMA_LEN; point++)
		gamma[point] = calculated[point];

	return 0;
}
