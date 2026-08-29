/* SPDX-License-Identifier: GPL-2.0-only */
// Portable-source SHA-256: a3ffa5f305e7230cf8784979485084ffc51638227f1e33e4ad21164fbf4c0ed2
#ifndef S6E8AA5X01_DIMMING_H
#define S6E8AA5X01_DIMMING_H

#include "s6e8aa5x01_gamma.h"
#include "s6e8aa5x01_tables.h"

#include <linux/types.h>

struct s6e8aa5x01_dimming_desc {
	const char *name;
	const s32 *target_coefficient;
	const s32 *maximum_target_coefficient;
	const s32 *search_curve;
	const u16 *base_luminance;
	const s16 (*gradation)[S6E8AA5X01_NUM_VOLTAGE_POINTS];
	const s16 (*rgb)[S6E8AA5X01_NUM_RGB_CORRECTIONS];
};

struct s6e8aa5x01_dimming {
	bool valid;
	const struct s6e8aa5x01_dimming_desc *desc;
	struct s6e8aa5x01_mtp mtp;
	struct s6e8aa5x01_voltages voltages;
	struct s6e8aa5x01_gray_table gray;
	u8 gamma[S6E8AA5X01_NUM_NORMAL_LEVELS]
		     [S6E8AA5X01_GAMMA_LEN];
};

extern const struct s6e8aa5x01_dimming_desc s6e8aa5x01_j5_a_desc;
extern const struct s6e8aa5x01_dimming_desc s6e8aa5x01_j5_c_desc;
extern const struct s6e8aa5x01_dimming_desc s6e8aa5x01_j5x_desc;

/* Structural variant permits synthetic zero MTP for oracle/unit testing. */
int s6e8aa5x01_dimming_init(struct s6e8aa5x01_dimming *dimming,
			    const struct s6e8aa5x01_dimming_desc *desc,
	const u8 mtp[S6E8AA5X01_MTP_LEN]);

/* Panel-driver boundary additionally rejects all-00/all-ff live reads. */
int s6e8aa5x01_dimming_init_live(struct s6e8aa5x01_dimming *dimming,
				 const struct s6e8aa5x01_dimming_desc *desc,
	const u8 mtp[S6E8AA5X01_MTP_LEN]);

const u8 *s6e8aa5x01_dimming_gamma(const struct s6e8aa5x01_dimming *dimming, unsigned int level);

#endif
