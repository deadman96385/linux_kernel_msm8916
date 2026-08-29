// SPDX-License-Identifier: GPL-2.0-only
// Portable-source SHA-256: 9531927b3d62978cedfad117773c6c920601932fdb06ca2277c44c1468f783a6
#include "s6e8aa5x01_gray.h"

#include <linux/errno.h>
#include <linux/types.h>

#define S6E8AA5X01_FIXED_SHIFT 22

struct s6e8aa5x01_gray_anchor {
	unsigned int gray;
	enum s6e8aa5x01_voltage_point point;
};

static const struct s6e8aa5x01_gray_anchor s6e8aa5x01_gray_anchors[] = {
	{ 3, S6E8AA5X01_V3 },
	{ 11, S6E8AA5X01_V11 },
	{ 23, S6E8AA5X01_V23 },
	{ 35, S6E8AA5X01_V35 },
	{ 51, S6E8AA5X01_V51 },
	{ 87, S6E8AA5X01_V87 },
	{ 151, S6E8AA5X01_V151 },
	{ 203, S6E8AA5X01_V203 },
	{ 255, S6E8AA5X01_V255 },
};

static void s6e8aa5x01_gray_segment(struct s6e8aa5x01_gray_table *gray,
				    size_t color, unsigned int first,
				   s32 high, unsigned int last,
				   s32 low)
{
	unsigned int span = last - first;
	unsigned int index;

	gray->value[first][color] = high;
	for (index = first + 1; index < last; index++) {
		u64 scaled;
		u64 increment;
		unsigned int numerator = last - index;

		/* Preserve Samsung's positive fixed-point division order exactly. */
		scaled = (u64)(high - low) * numerator;
		scaled <<= S6E8AA5X01_FIXED_SHIFT;
		scaled /= span;
		increment = scaled >> S6E8AA5X01_FIXED_SHIFT;
		gray->value[index][color] = low + (s32)increment;
	}
	gray->value[last][color] = low;
}

static int s6e8aa5x01_gray_validate(const struct s6e8aa5x01_voltages *voltages)
{
	size_t color;
	size_t i;

	for (color = 0; color < S6E8AA5X01_NUM_COLORS; color++) {
		unsigned int first = 0;
		s32 high = voltages->vregout;

		for (i = 0; i < sizeof(s6e8aa5x01_gray_anchors) /
				     sizeof(s6e8aa5x01_gray_anchors[0]); i++) {
			const struct s6e8aa5x01_gray_anchor *anchor =
				&s6e8aa5x01_gray_anchors[i];
			s32 low = voltages->anchor[anchor->point][color];

			if (anchor->gray <= first || high <= low)
				return -EDOM;
			first = anchor->gray;
			high = low;
		}
	}

	return 0;
}

int s6e8aa5x01_gray_init(struct s6e8aa5x01_gray_table *gray,
			 const struct s6e8aa5x01_voltages *voltages)
{
	size_t color;
	size_t i;
	int ret;

	if (!gray || !voltages || voltages->vregout <= 0)
		return -EINVAL;
	ret = s6e8aa5x01_gray_validate(voltages);
	if (ret)
		return ret;

	for (color = 0; color < S6E8AA5X01_NUM_COLORS; color++) {
		unsigned int first = 0;
		s32 high = voltages->vregout;

		for (i = 0; i < sizeof(s6e8aa5x01_gray_anchors) /
				     sizeof(s6e8aa5x01_gray_anchors[0]); i++) {
			const struct s6e8aa5x01_gray_anchor *anchor =
				&s6e8aa5x01_gray_anchors[i];
			s32 low = voltages->anchor[anchor->point][color];

			s6e8aa5x01_gray_segment(gray, color, first, high,
						anchor->gray, low);
			first = anchor->gray;
			high = low;
		}
	}

	return 0;
}
