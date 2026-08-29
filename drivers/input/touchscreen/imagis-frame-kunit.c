// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/string.h>

#include "imagis-frame.h"

#define IMAGIS_TEST_MAX_X	719
#define IMAGIS_TEST_MAX_Y	1279

static u32 imagis_test_coord(unsigned int x, unsigned int y,
			     unsigned int area)
{
	return FIELD_PREP(IST3038C_X_MASK, x) |
	       FIELD_PREP(IST3038C_Y_MASK, y) |
	       FIELD_PREP(IST3038C_AREA_MASK, area);
}

static u32 imagis_test_message(unsigned int finger_count,
			       unsigned int finger_status,
			       unsigned int key_count,
			       unsigned int key_status,
			       const u32 *coords)
{
	u32 message;

	message = IST3038C_INTR_STATUS_MASK |
		  FIELD_PREP(IST3038C_FINGER_COUNT_MASK, finger_count) |
		  FIELD_PREP(IST3038C_FINGER_STATUS_MASK, finger_status) |
		  FIELD_PREP(IST3032C_KEY_COUNT_MASK, key_count) |
		  FIELD_PREP(IST3032C_KEY_STATUS_MASK, key_status);
	message |= (u32)imagis_frame_checksum(message, coords, finger_count) << 24;

	return message;
}

static void imagis_frame_empty_test(struct kunit *test)
{
	struct imagis_frame frame;
	u32 message = imagis_test_message(0, 0, 0, 0, NULL);

	KUNIT_ASSERT_EQ(test,
			imagis_parse_frame(&frame, message, NULL, 0,
					   IMAGIS_TEST_MAX_X,
					   IMAGIS_TEST_MAX_Y, 0),
			0);
	KUNIT_EXPECT_EQ(test, frame.finger_count, (u8)0);
	KUNIT_EXPECT_EQ(test, frame.finger_status, (u16)0);
	KUNIT_EXPECT_EQ(test, frame.key_count, (u8)0);
	KUNIT_EXPECT_EQ(test, frame.key_status, (u8)0);
}

static void imagis_frame_sparse_slots_test(struct kunit *test)
{
	const u32 coords[] = {
		imagis_test_coord(10, 20, 3),
		imagis_test_coord(200, 300, 8),
		imagis_test_coord(719, 1279, 15),
	};
	const unsigned int status = BIT(1) | BIT(4) | BIT(9);
	struct imagis_frame frame;
	u32 message = imagis_test_message(ARRAY_SIZE(coords), status, 0, 0,
					  coords);

	KUNIT_ASSERT_EQ(test,
			imagis_parse_frame(&frame, message, coords,
					   ARRAY_SIZE(coords),
					   IMAGIS_TEST_MAX_X,
					   IMAGIS_TEST_MAX_Y, 0),
			0);
	KUNIT_EXPECT_EQ(test, frame.finger_count, (u8)ARRAY_SIZE(coords));
	KUNIT_EXPECT_EQ(test, frame.finger_status, (u16)status);
	KUNIT_EXPECT_EQ(test, frame.slot_for_coord[0], (u8)1);
	KUNIT_EXPECT_EQ(test, frame.slot_for_coord[1], (u8)4);
	KUNIT_EXPECT_EQ(test, frame.slot_for_coord[2], (u8)9);
}

static void imagis_frame_all_slots_test(struct kunit *test)
{
	u32 coords[IST3038C_MAX_FINGER_NUM];
	struct imagis_frame frame;
	u32 message;
	int i;

	for (i = 0; i < ARRAY_SIZE(coords); i++)
		coords[i] = imagis_test_coord(i * 10, i * 20, i);
	message = imagis_test_message(ARRAY_SIZE(coords), GENMASK(9, 0), 0, 0,
				      coords);

	KUNIT_ASSERT_EQ(test,
			imagis_parse_frame(&frame, message, coords,
					   ARRAY_SIZE(coords),
					   IMAGIS_TEST_MAX_X,
					   IMAGIS_TEST_MAX_Y, 0),
			0);
	for (i = 0; i < ARRAY_SIZE(coords); i++)
		KUNIT_EXPECT_EQ(test, frame.slot_for_coord[i], (u8)i);
}

static void imagis_frame_keys_test(struct kunit *test)
{
	struct imagis_frame frame;
	u32 message;

	message = imagis_test_message(0, 0, 2, BIT(0) | BIT(1), NULL);
	KUNIT_EXPECT_EQ(test,
			imagis_parse_frame(&frame, message, NULL, 0,
					   IMAGIS_TEST_MAX_X,
					   IMAGIS_TEST_MAX_Y, 2),
			0);
	KUNIT_EXPECT_EQ(test, frame.key_count, (u8)2);
	KUNIT_EXPECT_EQ(test, frame.key_status, (u8)(BIT(0) | BIT(1)));

	KUNIT_EXPECT_EQ(test,
			imagis_parse_frame(&frame, message, NULL, 0,
					   IMAGIS_TEST_MAX_X,
					   IMAGIS_TEST_MAX_Y, 0),
			-EPROTO);

	message = imagis_test_message(0, 0, 1, BIT(4), NULL);
	KUNIT_EXPECT_EQ(test,
			imagis_parse_frame(&frame, message, NULL, 0,
					   IMAGIS_TEST_MAX_X,
					   IMAGIS_TEST_MAX_Y, 2),
			-EPROTO);
}

static void imagis_frame_malformed_message_test(struct kunit *test)
{
	const u32 coords[] = {
		imagis_test_coord(10, 20, 3),
		imagis_test_coord(30, 40, 4),
	};
	struct imagis_frame frame;
	u32 message;

	message = imagis_test_message(2, BIT(0), 0, 0, coords);
	KUNIT_EXPECT_EQ(test,
			imagis_parse_frame(&frame, message, coords,
					   ARRAY_SIZE(coords),
					   IMAGIS_TEST_MAX_X,
					   IMAGIS_TEST_MAX_Y, 0),
			-EPROTO);

	message = imagis_test_message(2, BIT(0) | BIT(1), 0, 0, coords);
	KUNIT_EXPECT_EQ(test,
			imagis_parse_frame(&frame, message, coords, 1,
					   IMAGIS_TEST_MAX_X,
					   IMAGIS_TEST_MAX_Y, 0),
			-EMSGSIZE);

	message &= ~BIT(10);
	KUNIT_EXPECT_EQ(test,
			imagis_parse_frame(&frame, message, coords,
					   ARRAY_SIZE(coords),
					   IMAGIS_TEST_MAX_X,
					   IMAGIS_TEST_MAX_Y, 0),
			-EPROTO);

	message = IST3038C_INTR_STATUS_MASK |
		  FIELD_PREP(IST3038C_FINGER_COUNT_MASK, 11);
	KUNIT_EXPECT_EQ(test,
			imagis_parse_frame(&frame, message, NULL, 0,
					   IMAGIS_TEST_MAX_X,
					   IMAGIS_TEST_MAX_Y, 0),
			-EOVERFLOW);
}

static void imagis_frame_checksum_test(struct kunit *test)
{
	const u32 coords[] = { imagis_test_coord(100, 200, 7) };
	struct imagis_frame frame;
	u32 message = imagis_test_message(1, BIT(3), 0, 0, coords);

	message ^= BIT(24);
	KUNIT_EXPECT_EQ(test,
			imagis_parse_frame(&frame, message, coords,
					   ARRAY_SIZE(coords),
					   IMAGIS_TEST_MAX_X,
					   IMAGIS_TEST_MAX_Y, 0),
			-EBADMSG);
}

static void imagis_frame_bounds_test(struct kunit *test)
{
	struct imagis_frame frame;
	u32 coords[] = { imagis_test_coord(IMAGIS_TEST_MAX_X,
					 IMAGIS_TEST_MAX_Y, 1) };
	u32 message = imagis_test_message(1, BIT(0), 0, 0, coords);

	KUNIT_EXPECT_EQ(test,
			imagis_parse_frame(&frame, message, coords,
					   ARRAY_SIZE(coords),
					   IMAGIS_TEST_MAX_X,
					   IMAGIS_TEST_MAX_Y, 0),
			0);

	coords[0] = imagis_test_coord(IMAGIS_TEST_MAX_X + 1,
				      IMAGIS_TEST_MAX_Y, 1);
	message = imagis_test_message(1, BIT(0), 0, 0, coords);
	KUNIT_EXPECT_EQ(test,
			imagis_parse_frame(&frame, message, coords,
					   ARRAY_SIZE(coords),
					   IMAGIS_TEST_MAX_X,
					   IMAGIS_TEST_MAX_Y, 0),
			-ERANGE);
}

static void imagis_frame_transaction_test(struct kunit *test)
{
	const u32 coords[] = { imagis_test_coord(10, 20, 3) };
	struct imagis_frame before;
	struct imagis_frame frame;
	u32 message = imagis_test_message(1, BIT(0), 0, 0, coords) ^ BIT(24);

	memset(&frame, 0xa5, sizeof(frame));
	before = frame;

	KUNIT_ASSERT_EQ(test,
			imagis_parse_frame(&frame, message, coords,
					   ARRAY_SIZE(coords),
					   IMAGIS_TEST_MAX_X,
					   IMAGIS_TEST_MAX_Y, 0),
			-EBADMSG);
	KUNIT_EXPECT_EQ(test, memcmp(&frame, &before, sizeof(frame)), 0);

	KUNIT_EXPECT_EQ(test,
			imagis_parse_frame(NULL, message, coords,
					   ARRAY_SIZE(coords),
					   IMAGIS_TEST_MAX_X,
					   IMAGIS_TEST_MAX_Y, 0),
			-EINVAL);
	KUNIT_EXPECT_EQ(test,
			imagis_parse_frame(&frame, message, NULL,
					   ARRAY_SIZE(coords),
					   IMAGIS_TEST_MAX_X,
					   IMAGIS_TEST_MAX_Y, 0),
			-EINVAL);
}

static struct kunit_case imagis_frame_test_cases[] = {
	KUNIT_CASE(imagis_frame_empty_test),
	KUNIT_CASE(imagis_frame_sparse_slots_test),
	KUNIT_CASE(imagis_frame_all_slots_test),
	KUNIT_CASE(imagis_frame_keys_test),
	KUNIT_CASE(imagis_frame_malformed_message_test),
	KUNIT_CASE(imagis_frame_checksum_test),
	KUNIT_CASE(imagis_frame_bounds_test),
	KUNIT_CASE(imagis_frame_transaction_test),
	{ }
};

static struct kunit_suite imagis_frame_test_suite = {
	.name = "imagis-frame",
	.test_cases = imagis_frame_test_cases,
};

kunit_test_suite(imagis_frame_test_suite);

MODULE_DESCRIPTION("KUnit tests for the Imagis protocol B frame parser");
MODULE_LICENSE("GPL");
