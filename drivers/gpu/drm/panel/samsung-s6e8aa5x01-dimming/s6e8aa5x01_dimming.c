// SPDX-License-Identifier: GPL-2.0-only
// Portable-source SHA-256: 6072b9654299583c211ee19cb480a371b06f401da2a2f14fbf08cc557d8aef93
#include "s6e8aa5x01_dimming.h"

#include "s6e8aa5x01_curve.h"

#include <linux/errno.h>
#include <linux/types.h>
#include <linux/string.h>

const struct s6e8aa5x01_dimming_desc s6e8aa5x01_j5_a_desc = {
	.name = "AMS497HY01 revision A",
	.target_coefficient = s6e8aa5x01_coeff_2p0,
	.maximum_target_coefficient = s6e8aa5x01_coeff_2p0,
	.search_curve = s6e8aa5x01_curve_2p0_360,
	.base_luminance = s6e8aa5x01_j5_a_base,
	.gradation = s6e8aa5x01_j5_a_gradation,
	.rgb = s6e8aa5x01_j5_a_rgb,
};

const struct s6e8aa5x01_dimming_desc s6e8aa5x01_j5_c_desc = {
	.name = "AMS497HY01 revision C",
	.target_coefficient = s6e8aa5x01_coeff_2p0,
	.maximum_target_coefficient = s6e8aa5x01_coeff_2p0,
	.search_curve = s6e8aa5x01_curve_2p0_360,
	.base_luminance = s6e8aa5x01_j5_c_base,
	.gradation = s6e8aa5x01_j5_c_gradation,
	.rgb = s6e8aa5x01_j5_c_rgb,
};

const struct s6e8aa5x01_dimming_desc s6e8aa5x01_j5x_desc = {
	.name = "AMS520KT01",
	.target_coefficient = s6e8aa5x01_coeff_2p15,
	.maximum_target_coefficient = s6e8aa5x01_coeff_2p2,
	.search_curve = s6e8aa5x01_curve_2p2_360,
	.base_luminance = s6e8aa5x01_j5x_base,
	.gradation = s6e8aa5x01_j5x_gradation,
	.rgb = s6e8aa5x01_j5x_rgb,
};

static int s6e8aa5x01_desc_validate(const struct s6e8aa5x01_dimming_desc *desc)
{
	size_t level;

	if (!desc || !desc->name || !desc->target_coefficient ||
	    !desc->maximum_target_coefficient || !desc->search_curve ||
	    !desc->base_luminance || !desc->gradation || !desc->rgb)
		return -EINVAL;

	for (level = 0; level < S6E8AA5X01_NUM_NORMAL_LEVELS; level++) {
		if (!desc->base_luminance[level])
			return -EINVAL;
		if (level && s6e8aa5x01_candela[level] <=
			     s6e8aa5x01_candela[level - 1])
			return -EINVAL;
	}

	return 0;
}

static int s6e8aa5x01_dimming_init_decoded(struct s6e8aa5x01_dimming *dimming,
					   const struct s6e8aa5x01_dimming_desc *desc)
{
	struct s6e8aa5x01_curve_desc curve = {
		.search_curve = desc->search_curve,
		.count = S6E8AA5X01_NUM_GRAY_LEVELS,
	};
	size_t level;
	int ret;

	ret = s6e8aa5x01_voltage_init(&dimming->voltages, &s6e8aa5x01_j5_j5x_voltage_desc,
				      &dimming->mtp);
	if (ret)
		return ret;

	ret = s6e8aa5x01_gray_init(&dimming->gray, &dimming->voltages);
	if (ret)
		return ret;

	for (level = 0; level < S6E8AA5X01_NUM_NORMAL_LEVELS - 1;
	     level++) {
		s16 correction[S6E8AA5X01_NUM_VOLTAGE_POINTS];
		u16 index[S6E8AA5X01_NUM_VOLTAGE_POINTS];
		u8 raw[S6E8AA5X01_GAMMA_LEN];
		size_t point;

		curve.target_coefficient = desc->target_coefficient;
		for (point = 0; point < S6E8AA5X01_NUM_VOLTAGE_POINTS;
		     point++)
			correction[point] =
				desc->gradation[level][S6E8AA5X01_V255 - point];

		ret = s6e8aa5x01_gamma_indices(index, &curve,
					       desc->base_luminance[level], correction);
		if (ret)
			return ret;
		ret = s6e8aa5x01_gamma_reverse(raw, &s6e8aa5x01_j5_j5x_voltage_desc,
					       &dimming->voltages, &dimming->gray, index);
		if (ret)
			return ret;
		ret = s6e8aa5x01_gamma_compensate(dimming->gamma[level], raw, &dimming->mtp,
						  desc->rgb[level]);
		if (ret)
			return ret;
	}

	ret = s6e8aa5x01_gamma_center(dimming->gamma[S6E8AA5X01_NUM_NORMAL_LEVELS - 1],
				      &s6e8aa5x01_j5_j5x_voltage_desc);
	if (ret)
		return ret;

	dimming->desc = desc;
	dimming->valid = true;
	return 0;
}

static int s6e8aa5x01_dimming_init_common(struct s6e8aa5x01_dimming *dimming,
					  const struct s6e8aa5x01_dimming_desc *desc,
	const u8 mtp[S6E8AA5X01_MTP_LEN], bool live)
{
	int ret;

	if (!dimming || !mtp)
		return -EINVAL;

	memset(dimming, 0, sizeof(*dimming));
	ret = s6e8aa5x01_desc_validate(desc);
	if (!ret && live)
		ret = s6e8aa5x01_mtp_decode_live(&dimming->mtp, mtp, S6E8AA5X01_MTP_LEN);
	else if (!ret)
		ret = s6e8aa5x01_mtp_decode(&dimming->mtp, mtp, S6E8AA5X01_MTP_LEN);
	if (!ret)
		ret = s6e8aa5x01_dimming_init_decoded(dimming, desc);
	if (ret)
		memset(dimming, 0, sizeof(*dimming));

	return ret;
}

int s6e8aa5x01_dimming_init(struct s6e8aa5x01_dimming *dimming,
			    const struct s6e8aa5x01_dimming_desc *desc,
	const u8 mtp[S6E8AA5X01_MTP_LEN])
{
	return s6e8aa5x01_dimming_init_common(dimming, desc, mtp, false);
}

int s6e8aa5x01_dimming_init_live(struct s6e8aa5x01_dimming *dimming,
				 const struct s6e8aa5x01_dimming_desc *desc,
	const u8 mtp[S6E8AA5X01_MTP_LEN])
{
	return s6e8aa5x01_dimming_init_common(dimming, desc, mtp, true);
}

const u8 *s6e8aa5x01_dimming_gamma(const struct s6e8aa5x01_dimming *dimming, unsigned int level)
{
	if (!dimming || !dimming->valid ||
	    level >= S6E8AA5X01_NUM_NORMAL_LEVELS)
		return NULL;

	return dimming->gamma[level];
}
