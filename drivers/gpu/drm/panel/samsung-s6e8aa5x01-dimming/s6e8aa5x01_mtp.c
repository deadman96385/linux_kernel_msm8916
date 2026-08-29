// SPDX-License-Identifier: GPL-2.0-only
// Portable-source SHA-256: 2ca8249395e25946379b5d0ac737c8905fbefe5c2288c8d0195508f32fcd82c8
#include <linux/kernel.h>
#include "s6e8aa5x01_mtp.h"

#include <linux/errno.h>
#include <linux/types.h>

static s16 s6e8aa5x01_decode_smag8(u8 value)
{
	s16 magnitude = value & 0x7f;

	return value & 0x80 ? -magnitude : magnitude;
}

static bool s6e8aa5x01_uniform(const u8 *data, u8 value)
{
	size_t i;

	for (i = 0; i < S6E8AA5X01_MTP_LEN; i++)
		if (data[i] != value)
			return false;

	return true;
}

int s6e8aa5x01_mtp_decode(struct s6e8aa5x01_mtp *mtp,
			  const u8 *data, size_t len)
{
	struct s6e8aa5x01_mtp decoded = { 0 };
	static const enum s6e8aa5x01_voltage_point byte_points[] = {
		S6E8AA5X01_V203,
		S6E8AA5X01_V151,
		S6E8AA5X01_V87,
		S6E8AA5X01_V51,
		S6E8AA5X01_V35,
		S6E8AA5X01_V23,
		S6E8AA5X01_V11,
		S6E8AA5X01_V3,
	};
	size_t byte = 0;
	size_t point;
	size_t color;

	if (!mtp || !data || len != S6E8AA5X01_MTP_LEN)
		return -EINVAL;

	for (color = 0; color < S6E8AA5X01_NUM_COLORS; color++) {
		u8 sign = data[byte++];
		u8 magnitude = data[byte++];

		if (sign > 1)
			return -EINVAL;

		decoded.offset[S6E8AA5X01_V255][color] =
			sign ? -(s16)magnitude : magnitude;
	}

	for (point = 0; point < ARRAY_SIZE(byte_points);
	     point++)
		for (color = 0; color < S6E8AA5X01_NUM_COLORS; color++)
			decoded.offset[byte_points[point]][color] =
				s6e8aa5x01_decode_smag8(data[byte++]);

	for (color = 0; color < S6E8AA5X01_NUM_COLORS; color++) {
		u8 index = data[byte++];

		if (index > 15)
			return -ERANGE;

		decoded.vt_index[color] = index;
	}

	*mtp = decoded;
	return 0;
}

int s6e8aa5x01_mtp_decode_live(struct s6e8aa5x01_mtp *mtp,
			       const u8 *data, size_t len)
{
	if (!mtp || !data || len != S6E8AA5X01_MTP_LEN)
		return -EINVAL;

	if (s6e8aa5x01_uniform(data, 0x00) ||
	    s6e8aa5x01_uniform(data, 0xff))
		return -ENODATA;

	return s6e8aa5x01_mtp_decode(mtp, data, len);
}
