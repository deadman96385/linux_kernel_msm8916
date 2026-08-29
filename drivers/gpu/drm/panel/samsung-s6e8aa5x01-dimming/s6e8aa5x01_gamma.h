/* SPDX-License-Identifier: GPL-2.0-only */
// Portable-source SHA-256: 6ad97eec3dad741e3ecb4b7873b38f5ac3d142d204d9cf06140b900731fca4ad
#ifndef S6E8AA5X01_GAMMA_H
#define S6E8AA5X01_GAMMA_H

#include "s6e8aa5x01_gray.h"

#include <linux/types.h>

#define S6E8AA5X01_GAMMA_LEN 33
#define S6E8AA5X01_NUM_RGB_CORRECTIONS 24

/*
 * Reverse-convert checked gray indices into stock-compatible pre-compensation
 * bytes. Values intentionally use explicit modulo encoding here: stock can
 * produce a negative mathematical code which a later RGB correction brings
 * back into the legal wire range.
 */
int s6e8aa5x01_gamma_reverse(u8 gamma[S6E8AA5X01_GAMMA_LEN],
			     const struct s6e8aa5x01_voltage_desc *desc,
	const struct s6e8aa5x01_voltages *voltages,
	const struct s6e8aa5x01_gray_table *gray,
	const u16 index[S6E8AA5X01_NUM_VOLTAGE_POINTS]);

/* Apply one descriptor RGB row and subtract decoded MTP calibration. */
int s6e8aa5x01_gamma_compensate(u8 gamma[S6E8AA5X01_GAMMA_LEN],
				const u8 raw[S6E8AA5X01_GAMMA_LEN],
	const struct s6e8aa5x01_mtp *mtp,
	const s16 rgb[S6E8AA5X01_NUM_RGB_CORRECTIONS]);

/* Encode the descriptor center cell used by stock at maximum luminance. */
int s6e8aa5x01_gamma_center(u8 gamma[S6E8AA5X01_GAMMA_LEN],
			    const struct s6e8aa5x01_voltage_desc *desc);

#endif
