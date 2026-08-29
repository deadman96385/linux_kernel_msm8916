// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>

#include "s6e8aa5x01_dimming.h"
#include "s6e8aa5x01_policy.h"

static const u8 zero_denominator_mtp[S6E8AA5X01_MTP_LEN] = {
	0x01, 0xc4, 0x00, 0x68, 0x01, 0x8e,
	0x24, 0xc3, 0xec, 0xf1, 0x0e, 0xca,
	0xf6, 0xd3, 0x89, 0x62, 0x6e, 0x70,
	0x0c, 0x28, 0x90, 0x39, 0x2d, 0x7a,
	0x5a, 0xe0, 0x3b, 0xed, 0x8c, 0x1c,
	0x0f, 0x02, 0x0f,
};

static void s6e8aa5x01_mtp_test(struct kunit *test)
{
	struct s6e8aa5x01_mtp decoded;
	u8 mtp[S6E8AA5X01_MTP_LEN] = { 0 };

	KUNIT_EXPECT_EQ(test, s6e8aa5x01_mtp_decode(&decoded, mtp,
						    sizeof(mtp)), 0);
	KUNIT_EXPECT_EQ(test, decoded.offset[S6E8AA5X01_V255]
					    [S6E8AA5X01_RED], (s16)0);
	KUNIT_EXPECT_EQ(test, s6e8aa5x01_mtp_decode_live(&decoded, mtp,
							 sizeof(mtp)),
			-ENODATA);

	mtp[0] = 2;
	KUNIT_EXPECT_EQ(test, s6e8aa5x01_mtp_decode(&decoded, mtp,
						    sizeof(mtp)), -EINVAL);
	mtp[0] = 0;
	mtp[30] = 16;
	KUNIT_EXPECT_EQ(test, s6e8aa5x01_mtp_decode(&decoded, mtp,
						    sizeof(mtp)), -ERANGE);
}

static void s6e8aa5x01_dimming_test(struct kunit *test)
{
	static const u8 zero_mtp[S6E8AA5X01_MTP_LEN];
	static const u8 j5a_first[S6E8AA5X01_GAMMA_LEN] = {
		0x00, 0x27, 0x00, 0x27, 0x00, 0x28,
		0xcb, 0xcb, 0xcc, 0xb3, 0xb3, 0xb4,
		0x81, 0x83, 0x83, 0x64, 0x69, 0x6c,
		0x9e, 0xa2, 0xa0, 0xc6, 0xd2, 0xcd,
		0xc3, 0xd2, 0xd1, 0x7b, 0x7b, 0x7b,
		0x00, 0x00, 0x00,
	};
	static const u8 j5c_first[S6E8AA5X01_GAMMA_LEN] = {
		0x00, 0x2a, 0x00, 0x28, 0x00, 0x29,
		0xc9, 0xc9, 0xca, 0xb2, 0xb3, 0xb5,
		0x7c, 0x7f, 0x7f, 0x67, 0x6c, 0x70,
		0x8f, 0x95, 0x92, 0xd2, 0xe0, 0xde,
		0xc4, 0xc4, 0xc5, 0x91, 0x91, 0x91,
		0x00, 0x00, 0x00,
	};
	static const u8 j5x_first[S6E8AA5X01_GAMMA_LEN] = {
		0x00, 0x2d, 0x00, 0x2c, 0x00, 0x2c,
		0xc8, 0xc9, 0xc9, 0xb4, 0xb4, 0xb5,
		0x83, 0x85, 0x85, 0x77, 0x7a, 0x7d,
		0xa4, 0xab, 0xac, 0xc2, 0xc8, 0xc8,
		0xd5, 0xda, 0xd8, 0xdd, 0xdd, 0xdd,
		0x00, 0x00, 0x00,
	};
	struct s6e8aa5x01_dimming *dimming;
	const u8 *gamma;

	dimming = kunit_kzalloc(test, sizeof(*dimming), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, dimming);

	KUNIT_ASSERT_EQ(test, s6e8aa5x01_dimming_init(dimming, &s6e8aa5x01_j5_a_desc,
						      zero_mtp), 0);
	gamma = s6e8aa5x01_dimming_gamma(dimming, 0);
	KUNIT_ASSERT_NOT_NULL(test, gamma);
	KUNIT_EXPECT_MEMEQ(test, gamma, j5a_first, sizeof(j5a_first));

	KUNIT_ASSERT_EQ(test, s6e8aa5x01_dimming_init(dimming, &s6e8aa5x01_j5_c_desc,
						      zero_mtp), 0);
	KUNIT_EXPECT_MEMEQ(test, s6e8aa5x01_dimming_gamma(dimming, 0),
			   j5c_first, sizeof(j5c_first));

	KUNIT_ASSERT_EQ(test, s6e8aa5x01_dimming_init(dimming, &s6e8aa5x01_j5x_desc,
						      zero_mtp), 0);
	KUNIT_EXPECT_MEMEQ(test, s6e8aa5x01_dimming_gamma(dimming, 0),
			   j5x_first, sizeof(j5x_first));

	KUNIT_EXPECT_EQ(test, s6e8aa5x01_dimming_init(dimming, &s6e8aa5x01_j5x_desc,
						      zero_denominator_mtp), -EDOM);
	KUNIT_EXPECT_FALSE(test, dimming->valid);
}

static void s6e8aa5x01_policy_test(struct kunit *test)
{
	struct s6e8aa5x01_temperature_result result;
	u8 level;

	KUNIT_ASSERT_EQ(test,
			s6e8aa5x01_policy_validate(&s6e8aa5x01_j5_a_policy),
			0);
	KUNIT_ASSERT_EQ(test,
			s6e8aa5x01_policy_validate(&s6e8aa5x01_j5_c_policy),
			0);
	KUNIT_ASSERT_EQ(test,
			s6e8aa5x01_policy_validate(&s6e8aa5x01_j5x_policy),
			0);

	KUNIT_ASSERT_EQ(test, s6e8aa5x01_policy_level(&s6e8aa5x01_j5x_policy, 71, &level), 0);
	KUNIT_EXPECT_EQ(test, level, (u8)40);
	KUNIT_ASSERT_EQ(test, s6e8aa5x01_policy_level(&s6e8aa5x01_j5x_policy, 97, &level), 0);
	KUNIT_EXPECT_EQ(test, level, (u8)45);

	KUNIT_ASSERT_EQ(test, s6e8aa5x01_temperature_resolve(&result, &s6e8aa5x01_j5_a_policy,
							     0, -20, false, 0), 0);
	KUNIT_EXPECT_EQ(test, result.encoded_temperature, (u8)0x94);
	KUNIT_EXPECT_EQ(test, result.elvss, (u8)0x13);
	KUNIT_EXPECT_FALSE(test, result.uses_factory_elvss);

	KUNIT_EXPECT_EQ(test, s6e8aa5x01_temperature_resolve(&result, &s6e8aa5x01_j5_a_policy,
							     21, 20, false, 0), -ENODATA);
}

static struct kunit_case s6e8aa5x01_test_cases[] = {
	KUNIT_CASE(s6e8aa5x01_mtp_test),
	KUNIT_CASE(s6e8aa5x01_dimming_test),
	KUNIT_CASE(s6e8aa5x01_policy_test),
	{ }
};

static struct kunit_suite s6e8aa5x01_test_suite = {
	.name = "s6e8aa5x01-dimming",
	.test_cases = s6e8aa5x01_test_cases,
};

kunit_test_suite(s6e8aa5x01_test_suite);

MODULE_DESCRIPTION("KUnit tests for Samsung S6E8AA5X01 smart dimming");
MODULE_LICENSE("GPL");
