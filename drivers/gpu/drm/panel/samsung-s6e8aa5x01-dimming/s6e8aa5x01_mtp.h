/* SPDX-License-Identifier: GPL-2.0-only */
// Portable-source SHA-256: 723176ce46117a2fb6b01a262cfb08d27e9d2da009a0cd2bd2f8b2f5671c2c6a
#ifndef S6E8AA5X01_MTP_H
#define S6E8AA5X01_MTP_H

#include <linux/types.h>

#define S6E8AA5X01_MTP_LEN 33

enum s6e8aa5x01_color {
	S6E8AA5X01_RED,
	S6E8AA5X01_GREEN,
	S6E8AA5X01_BLUE,
	S6E8AA5X01_NUM_COLORS,
};

/* Ordered by increasing voltage point for calculation-table indexing. */
enum s6e8aa5x01_voltage_point {
	S6E8AA5X01_V3,
	S6E8AA5X01_V11,
	S6E8AA5X01_V23,
	S6E8AA5X01_V35,
	S6E8AA5X01_V51,
	S6E8AA5X01_V87,
	S6E8AA5X01_V151,
	S6E8AA5X01_V203,
	S6E8AA5X01_V255,
	S6E8AA5X01_NUM_VOLTAGE_POINTS,
};

struct s6e8aa5x01_mtp {
	s16 offset[S6E8AA5X01_NUM_VOLTAGE_POINTS]
		      [S6E8AA5X01_NUM_COLORS];
	u8 vt_index[S6E8AA5X01_NUM_COLORS];
};

/*
 * Decode structurally valid C8 data. Synthetic all-zero input is allowed so
 * arithmetic tests can use it; panel drivers should call the live variant.
 * The output is modified only on success.
 */
int s6e8aa5x01_mtp_decode(struct s6e8aa5x01_mtp *mtp,
			  const u8 *data, size_t len);

/*
 * Decode a live panel read, additionally rejecting uniform 00/ff blocks as a
 * failed or mistimed DSI transfer. The output is modified only on success.
 */
int s6e8aa5x01_mtp_decode_live(struct s6e8aa5x01_mtp *mtp,
			       const u8 *data, size_t len);

#endif
