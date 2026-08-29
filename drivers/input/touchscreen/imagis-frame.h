/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef _IMAGIS_FRAME_H
#define _IMAGIS_FRAME_H

#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/bits.h>
#include <linux/errno.h>
#include <linux/types.h>

#define IST3038C_MAX_FINGER_NUM		10
#define IST3038C_MAX_KEY_NUM		5
#define IST3038C_X_MASK			GENMASK(23, 12)
#define IST3038C_Y_MASK			GENMASK(11, 0)
#define IST3038C_AREA_MASK		GENMASK(27, 24)
#define IST3038C_FINGER_COUNT_MASK	GENMASK(15, 12)
#define IST3038C_FINGER_STATUS_MASK	GENMASK(9, 0)
#define IST3032C_KEY_STATUS_MASK		GENMASK(20, 16)
#define IST3032C_KEY_COUNT_MASK		GENMASK(23, 21)
#define IST3038C_INTR_STATUS_MASK	GENMASK(11, 10)
#define IST3038C_INTR_CHECKSUM_MASK	GENMASK(31, 24)

struct imagis_frame {
	u16 finger_status;
	u8 key_status;
	u8 finger_count;
	u8 key_count;
	u8 slot_for_coord[IST3038C_MAX_FINGER_NUM];
};

static inline u8 imagis_frame_checksum(u32 intr_message, const u32 *coords,
				       unsigned int coord_count)
{
	u8 checksum = intr_message;
	unsigned int i;

	checksum += intr_message >> 8;
	checksum += intr_message >> 16;

	for (i = 0; i < coord_count; i++) {
		checksum += coords[i];
		checksum += coords[i] >> 8;
		checksum += coords[i] >> 16;
		checksum += coords[i] >> 24;
	}

	return checksum;
}

static inline int imagis_parse_frame(struct imagis_frame *frame,
				     u32 intr_message, const u32 *coords,
				     unsigned int coord_count,
				     unsigned int max_x, unsigned int max_y,
				     unsigned int max_keys)
{
	struct imagis_frame parsed = { };
	unsigned int expected_checksum;
	unsigned int slot;
	unsigned int i;

	if (!frame || (coord_count && !coords) ||
	    max_keys > IST3038C_MAX_KEY_NUM)
		return -EINVAL;

	if ((intr_message & IST3038C_INTR_STATUS_MASK) !=
	    IST3038C_INTR_STATUS_MASK)
		return -EPROTO;

	parsed.finger_count = FIELD_GET(IST3038C_FINGER_COUNT_MASK,
					intr_message);
	parsed.finger_status = FIELD_GET(IST3038C_FINGER_STATUS_MASK,
					 intr_message);
	parsed.key_count = FIELD_GET(IST3032C_KEY_COUNT_MASK, intr_message);
	parsed.key_status = FIELD_GET(IST3032C_KEY_STATUS_MASK, intr_message);

	if (parsed.finger_count > IST3038C_MAX_FINGER_NUM)
		return -EOVERFLOW;
	if (coord_count != parsed.finger_count)
		return -EMSGSIZE;
	if (hweight16(parsed.finger_status) != parsed.finger_count)
		return -EPROTO;
	if (parsed.key_count > max_keys ||
	    hweight8(parsed.key_status) != parsed.key_count ||
	    parsed.key_status & ~((1U << max_keys) - 1))
		return -EPROTO;

	expected_checksum = FIELD_GET(IST3038C_INTR_CHECKSUM_MASK,
				      intr_message);
	if (imagis_frame_checksum(intr_message, coords, coord_count) !=
	    expected_checksum)
		return -EBADMSG;

	for (i = 0; i < coord_count; i++) {
		if (FIELD_GET(IST3038C_X_MASK, coords[i]) > max_x ||
		    FIELD_GET(IST3038C_Y_MASK, coords[i]) > max_y)
			return -ERANGE;
	}

	i = 0;
	for (slot = 0; slot < IST3038C_MAX_FINGER_NUM; slot++) {
		if (parsed.finger_status & BIT(slot))
			parsed.slot_for_coord[i++] = slot;
	}

	*frame = parsed;

	return 0;
}

#endif /* _IMAGIS_FRAME_H */
