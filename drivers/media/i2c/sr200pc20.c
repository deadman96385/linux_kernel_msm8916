// SPDX-License-Identifier: GPL-2.0-only
/*
 * V4L2 driver for the SiliconFile SR200PC20 image sensor.
 *
 * This deliberately implements only the native GTEL preview path. The
 * register sequence is the 26 MHz, 60 Hz, 800x600 YUV sequence shipped in
 * Samsung's downstream SM-T560NU kernel.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>

#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-mediabus.h>
#include <media/v4l2-subdev.h>

#define SR200PC20_CHIP_ID_REG		CCI_REG8(0x04)
#define SR200PC20_CHIP_ID		0xb4
#define SR200PC20_PAGE_SELECT		CCI_REG8(0x03)
#define SR200PC20_SLEEP			CCI_REG8(0x01)

#define SR200PC20_XCLK_FREQ		26000000
#define SR200PC20_XCLK_MIN		25900000
#define SR200PC20_XCLK_MAX		26100000
#define SR200PC20_LINK_FREQ		144000000
#define SR200PC20_PIXEL_RATE		18000000
#define SR200PC20_WIDTH			800
#define SR200PC20_HEIGHT			600

struct sr200pc20 {
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct v4l2_ctrl_handler ctrls;
	struct regmap *regmap;

	struct clk *xclk;
	struct regulator *vdig;
	struct regulator *vana;
	struct regulator *vio;
	struct gpio_desc *reset_gpio;
	struct gpio_desc *standby_gpio;

	s64 link_freq;
	bool streaming;
};

static inline struct sr200pc20 *to_sr200pc20(struct v4l2_subdev *sd)
{
	return container_of(sd, struct sr200pc20, sd);
}

static const struct cci_reg_sequence sr200pc20_800x600_60hz[] = {
/* 01. Start Setting */
{ CCI_REG8(0x01), 0x31 }, /* sleep on */
{ CCI_REG8(0x01), 0x33 }, /* sleep on */
{ CCI_REG8(0x01), 0x31 }, /* sleep on */
{ CCI_REG8(0x08), 0x2f }, /* sleep on */
{ CCI_REG8(0x0a), 0x00 }, /* sleep on */

/* PAGE 20 */
{ CCI_REG8(0x03), 0x20 }, /* page 20 */
{ CCI_REG8(0x10), 0x0c }, /* AE off 60hz */

/* PAGE 22 */
{ CCI_REG8(0x03), 0x22 }, /* page 22 */
{ CCI_REG8(0x10), 0x69 }, /* AWB off */

{ CCI_REG8(0x03), 0x12 },
{ CCI_REG8(0x20), 0x00 },
{ CCI_REG8(0x21), 0x00 },

{ CCI_REG8(0x03), 0x13 },
{ CCI_REG8(0x10), 0xcb },

/* Initial Start */
/* PAGE 0 START */
{ CCI_REG8(0x03), 0x00 },
{ CCI_REG8(0x10), 0x11 }, /* Vsync Active High B:[3], Sub1/2 + Preview 1mode */
{ CCI_REG8(0x11), 0x90 },
{ CCI_REG8(0x12), 0x04 }, /* Pclk Falling Edge B:[2] */

{ CCI_REG8(0x0b), 0xaa }, /* ESD Check Register */
{ CCI_REG8(0x0c), 0xaa }, /* ESD Check Register */
{ CCI_REG8(0x0d), 0xaa }, /* ESD Check Register */

{ CCI_REG8(0x20), 0x00 },
{ CCI_REG8(0x21), 0x02 }, /* modify 20110929, 0x04 -> 0x02 */
{ CCI_REG8(0x22), 0x00 },
{ CCI_REG8(0x23), 0x0a }, /* modify 20110929, 0x14 -> 0x0a */

{ CCI_REG8(0x24), 0x04 },
{ CCI_REG8(0x25), 0xb0 },
{ CCI_REG8(0x26), 0x06 },
{ CCI_REG8(0x27), 0x40 },

{ CCI_REG8(0x28), 0x0c },
{ CCI_REG8(0x29), 0x04 },
{ CCI_REG8(0x2a), 0x02 },
{ CCI_REG8(0x2b), 0x04 },
{ CCI_REG8(0x2c), 0x06 },
{ CCI_REG8(0x2d), 0x02 },

{ CCI_REG8(0x40), 0x01 }, /* Hblank_360 */
{ CCI_REG8(0x41), 0x68 },
{ CCI_REG8(0x42), 0x00 },
{ CCI_REG8(0x43), 0x7e }, /* 126 */

{ CCI_REG8(0x44), 0x09 }, /* VSCLIP */

{ CCI_REG8(0x45), 0x04 },
{ CCI_REG8(0x46), 0x18 },
{ CCI_REG8(0x47), 0xd8 },

/* BLC */
{ CCI_REG8(0x80), 0x2e },
{ CCI_REG8(0x81), 0x7c }, /* 2frame -> 8 frame */
{ CCI_REG8(0x82), 0x90 },
{ CCI_REG8(0x83), 0x00 },
{ CCI_REG8(0x84), 0x0c },
{ CCI_REG8(0x85), 0x00 },
{ CCI_REG8(0x90), 0x0f }, /* BLC_TIME_TH_ON */
{ CCI_REG8(0x91), 0x0f }, /* BLC_TIME_TH_OFF */
{ CCI_REG8(0x92), 0xa8 }, /* BLC_AG_TH_ON */
{ CCI_REG8(0x93), 0xa0 }, /* BLC_AG_TH_OFF */
{ CCI_REG8(0x94), 0xff },
{ CCI_REG8(0x95), 0xff },
{ CCI_REG8(0x96), 0xdc },
{ CCI_REG8(0x97), 0xfe },
{ CCI_REG8(0x98), 0x38 },

/* Dark BLC */
{ CCI_REG8(0xa0), 0x00 },
{ CCI_REG8(0xa2), 0x00 },
{ CCI_REG8(0xa4), 0x00 },
{ CCI_REG8(0xa6), 0x00 },

/* Normal BLC */
{ CCI_REG8(0xa8), 0x41 }, /* B */
{ CCI_REG8(0xaa), 0x41 },
{ CCI_REG8(0xac), 0x41 },
{ CCI_REG8(0xae), 0x41 },

/* OutDoor BLC */
{ CCI_REG8(0x99), 0x43 },
{ CCI_REG8(0x9a), 0x43 },
{ CCI_REG8(0x9b), 0x43 },
{ CCI_REG8(0x9c), 0x43 },
/* PAGE 0 END */

/* PAGE 2 START */
{ CCI_REG8(0x03), 0x02 },
{ CCI_REG8(0x12), 0x03 },
{ CCI_REG8(0x13), 0x03 },
{ CCI_REG8(0x16), 0x00 },
{ CCI_REG8(0x17), 0x8C },
{ CCI_REG8(0x18), 0x4c }, /* Double_AG */
{ CCI_REG8(0x19), 0x00 },
{ CCI_REG8(0x1a), 0x39 }, /* Double_AG 38 -> 39 */
{ CCI_REG8(0x1c), 0x09 },
{ CCI_REG8(0x1d), 0x40 },
{ CCI_REG8(0x1e), 0x30 },
{ CCI_REG8(0x1f), 0x10 },

{ CCI_REG8(0x20), 0x77 },
{ CCI_REG8(0x21), 0xde },
{ CCI_REG8(0x22), 0xa7 },
{ CCI_REG8(0x23), 0x30 }, /* CLAMP */
{ CCI_REG8(0x27), 0x3c },
{ CCI_REG8(0x2b), 0x80 },
{ CCI_REG8(0x2e), 0x00 },
{ CCI_REG8(0x2f), 0x00 },
{ CCI_REG8(0x30), 0x05 }, /* For Hi-253 never no change 0x05 */

{ CCI_REG8(0x50), 0x20 },
{ CCI_REG8(0x51), 0x1c }, /* add 20110826 */
{ CCI_REG8(0x52), 0x01 }, /* 0x03 -> 0x01 */
{ CCI_REG8(0x53), 0xc1 }, /* add 20110818 */
{ CCI_REG8(0x54), 0xc0 },
{ CCI_REG8(0x55), 0x1c },
{ CCI_REG8(0x56), 0x11 },
{ CCI_REG8(0x58), 0x22 }, /*add 20120430 */
{ CCI_REG8(0x59), 0x20 }, /*add 20120430 */
{ CCI_REG8(0x5d), 0xa2 },
{ CCI_REG8(0x5e), 0x5a },

{ CCI_REG8(0x60), 0x87 },
{ CCI_REG8(0x61), 0x99 },
{ CCI_REG8(0x62), 0x88 },
{ CCI_REG8(0x63), 0x97 },
{ CCI_REG8(0x64), 0x88 },
{ CCI_REG8(0x65), 0x97 },

{ CCI_REG8(0x67), 0x0c },
{ CCI_REG8(0x68), 0x0c },
{ CCI_REG8(0x69), 0x0c },

{ CCI_REG8(0x72), 0x89 },
{ CCI_REG8(0x73), 0x96 },
{ CCI_REG8(0x74), 0x89 },
{ CCI_REG8(0x75), 0x96 },
{ CCI_REG8(0x76), 0x89 },
{ CCI_REG8(0x77), 0x96 },

{ CCI_REG8(0x7c), 0x85 },
{ CCI_REG8(0x7d), 0xaf },
{ CCI_REG8(0x80), 0x01 },
{ CCI_REG8(0x81), 0x7f },
{ CCI_REG8(0x82), 0x13 },
{ CCI_REG8(0x83), 0x24 },
{ CCI_REG8(0x84), 0x7d },
{ CCI_REG8(0x85), 0x81 },
{ CCI_REG8(0x86), 0x7d },
{ CCI_REG8(0x87), 0x81 },

{ CCI_REG8(0x92), 0x48 },
{ CCI_REG8(0x93), 0x54 },
{ CCI_REG8(0x94), 0x7d },
{ CCI_REG8(0x95), 0x81 },
{ CCI_REG8(0x96), 0x7d },
{ CCI_REG8(0x97), 0x81 },

{ CCI_REG8(0xa0), 0x02 },
{ CCI_REG8(0xa1), 0x7b },
{ CCI_REG8(0xa2), 0x02 },
{ CCI_REG8(0xa3), 0x7b },
{ CCI_REG8(0xa4), 0x7b },
{ CCI_REG8(0xa5), 0x02 },
{ CCI_REG8(0xa6), 0x7b },
{ CCI_REG8(0xa7), 0x02 },

{ CCI_REG8(0xa8), 0x85 },
{ CCI_REG8(0xa9), 0x8c },
{ CCI_REG8(0xaa), 0x85 },
{ CCI_REG8(0xab), 0x8c },
{ CCI_REG8(0xac), 0x10 },
{ CCI_REG8(0xad), 0x16 },
{ CCI_REG8(0xae), 0x10 },
{ CCI_REG8(0xaf), 0x16 },

{ CCI_REG8(0xb0), 0x99 },
{ CCI_REG8(0xb1), 0xa3 },
{ CCI_REG8(0xb2), 0xa4 },
{ CCI_REG8(0xb3), 0xae },
{ CCI_REG8(0xb4), 0x9b },
{ CCI_REG8(0xb5), 0xa2 },
{ CCI_REG8(0xb6), 0xa6 },
{ CCI_REG8(0xb7), 0xac },
{ CCI_REG8(0xb8), 0x9b },
{ CCI_REG8(0xb9), 0x9f },
{ CCI_REG8(0xba), 0xa6 },
{ CCI_REG8(0xbb), 0xaa },
{ CCI_REG8(0xbc), 0x9b },
{ CCI_REG8(0xbd), 0x9f },
{ CCI_REG8(0xbe), 0xa6 },
{ CCI_REG8(0xbf), 0xaa },

{ CCI_REG8(0xc4), 0x2c },
{ CCI_REG8(0xc5), 0x43 },
{ CCI_REG8(0xc6), 0x63 },
{ CCI_REG8(0xc7), 0x79 },

{ CCI_REG8(0xc8), 0x2d },
{ CCI_REG8(0xc9), 0x42 },
{ CCI_REG8(0xca), 0x2d },
{ CCI_REG8(0xcb), 0x42 },
{ CCI_REG8(0xcc), 0x64 },
{ CCI_REG8(0xcd), 0x78 },
{ CCI_REG8(0xce), 0x64 },
{ CCI_REG8(0xcf), 0x78 },
{ CCI_REG8(0xd0), 0x0a },
{ CCI_REG8(0xd1), 0x09 },
{ CCI_REG8(0xd4), 0x0f }, /* DCDC_TIME_TH_ON */
{ CCI_REG8(0xd5), 0x0f }, /* DCDC_TIME_TH_OFF */
{ CCI_REG8(0xd6), 0xa8 }, /* DCDC_AG_TH_ON */
{ CCI_REG8(0xd7), 0xa0 }, /* DCDC_AG_TH_OFF */
{ CCI_REG8(0xe0), 0xc4 },
{ CCI_REG8(0xe1), 0xc4 },
{ CCI_REG8(0xe2), 0xc4 },
{ CCI_REG8(0xe3), 0xc4 },
{ CCI_REG8(0xe4), 0x00 },
{ CCI_REG8(0xe8), 0x80 },
{ CCI_REG8(0xe9), 0x40 },
{ CCI_REG8(0xea), 0x7f },

{ CCI_REG8(0xf0), 0x01 },
{ CCI_REG8(0xf1), 0x01 },
{ CCI_REG8(0xf2), 0x01 },
{ CCI_REG8(0xf3), 0x01 },
{ CCI_REG8(0xf4), 0x01 },
/* PAGE 2 END */

/* PAGE 3 */
{ CCI_REG8(0x03), 0x03 },
{ CCI_REG8(0x10), 0x10 },
/* PAGE 3 END */

/* PAGE 10 START */
{ CCI_REG8(0x03), 0x10 },
{ CCI_REG8(0x10), 0x01 }, /* 00: CrYCbY, 01: CbYCrY */
{ CCI_REG8(0x12), 0x30 },
{ CCI_REG8(0x20), 0x00 },
{ CCI_REG8(0x30), 0x00 },
{ CCI_REG8(0x31), 0x00 },
{ CCI_REG8(0x32), 0x00 },
{ CCI_REG8(0x33), 0x00 },

{ CCI_REG8(0x34), 0x30 },
{ CCI_REG8(0x35), 0x00 },
{ CCI_REG8(0x36), 0x00 },
{ CCI_REG8(0x38), 0x00 },
{ CCI_REG8(0x3e), 0x58 },

{ CCI_REG8(0x40), 0x80 },
{ CCI_REG8(0x41), 0x00 },

{ CCI_REG8(0x60), 0x6b },
{ CCI_REG8(0x61), 0x7b }, /* 77 */
{ CCI_REG8(0x62), 0x7b }, /* 77 */
{ CCI_REG8(0x63), 0xa0 }, /* Double_AG 50 -> 30 */
{ CCI_REG8(0x64), 0x80 },

{ CCI_REG8(0x66), 0x42 },
{ CCI_REG8(0x67), 0x00 },

{ CCI_REG8(0x6a), 0x8a }, /* 8a */
{ CCI_REG8(0x6b), 0x69 }, /* 74 20150320_71*/
{ CCI_REG8(0x6c), 0x78 }, /* 7e 20150320_6c*/
{ CCI_REG8(0x6d), 0x8e }, /* 8e */
{ CCI_REG8(0x76), 0x01 }, /* ADD 20120522 */
{ CCI_REG8(0x79), 0x04 }, /* ADD 20120522 */

/* PAGE 11 START */
{ CCI_REG8(0x03), 0x11 },
{ CCI_REG8(0x10), 0x7f },
{ CCI_REG8(0x11), 0x40 },
{ CCI_REG8(0x12), 0x0a }, /* Blue Max-Filter Delete */
{ CCI_REG8(0x13), 0xb9 },

{ CCI_REG8(0x26), 0x68 }, /* Double_AG 31 -> 20 */
{ CCI_REG8(0x27), 0x62 }, /* Double_AG 34 -> 22 */
{ CCI_REG8(0x28), 0x0f },
{ CCI_REG8(0x29), 0x10 },
{ CCI_REG8(0x2b), 0x30 },
{ CCI_REG8(0x2c), 0x32 },

/* Out2 D-LPF th */
{ CCI_REG8(0x30), 0x70 },
{ CCI_REG8(0x31), 0x10 },
{ CCI_REG8(0x32), 0x58 },
{ CCI_REG8(0x33), 0x09 },
{ CCI_REG8(0x34), 0x06 },
{ CCI_REG8(0x35), 0x03 },

/* Out1 D-LPF th */
{ CCI_REG8(0x36), 0x70 },
{ CCI_REG8(0x37), 0x18 },
{ CCI_REG8(0x38), 0x58 },
{ CCI_REG8(0x39), 0x20 },
{ CCI_REG8(0x3a), 0x1f },
{ CCI_REG8(0x3b), 0x03 },

/* Indoor D-LPF th */
{ CCI_REG8(0x3c), 0x80 },
{ CCI_REG8(0x3d), 0x18 },
{ CCI_REG8(0x3e), 0x80 },
{ CCI_REG8(0x3f), 0x0c },
{ CCI_REG8(0x40), 0x09 },
{ CCI_REG8(0x41), 0x06 },

/* Dark1 D-LPF th */
{ CCI_REG8(0x42), 0x80 },
{ CCI_REG8(0x43), 0x18 },
{ CCI_REG8(0x44), 0x80 },
{ CCI_REG8(0x45), 0x0f },
{ CCI_REG8(0x46), 0x0c },
{ CCI_REG8(0x47), 0x0d },

/* Dark2 D-LPF th */
{ CCI_REG8(0x48), 0x88 },
{ CCI_REG8(0x49), 0x2c },
{ CCI_REG8(0x4a), 0x80 },
{ CCI_REG8(0x4b), 0x0f },
{ CCI_REG8(0x4c), 0x0c },
{ CCI_REG8(0x4d), 0x0d },

/* Dark3 D-LPF th */
{ CCI_REG8(0x4e), 0x80 },
{ CCI_REG8(0x4f), 0x23 },
{ CCI_REG8(0x50), 0x80 },
{ CCI_REG8(0x51), 0x0f },
{ CCI_REG8(0x52), 0x0c },
{ CCI_REG8(0x53), 0x0c },

{ CCI_REG8(0x54), 0x11 },
{ CCI_REG8(0x55), 0x17 },
{ CCI_REG8(0x56), 0x20 },
{ CCI_REG8(0x57), 0x01 },
{ CCI_REG8(0x58), 0x00 },
{ CCI_REG8(0x59), 0x00 },

{ CCI_REG8(0x5a), 0x18 },
{ CCI_REG8(0x5b), 0x00 },
{ CCI_REG8(0x5c), 0x00 },

{ CCI_REG8(0x60), 0x3f },
{ CCI_REG8(0x62), 0x60 },
{ CCI_REG8(0x70), 0x06 },
/* PAGE 11 END */

/* PAGE 12 START */
{ CCI_REG8(0x03), 0x12 },
{ CCI_REG8(0x20), 0x00 },
{ CCI_REG8(0x21), 0x00 },

{ CCI_REG8(0x25), 0x00 }, /* 0x30 */

{ CCI_REG8(0x28), 0x00 },
{ CCI_REG8(0x29), 0x00 },
{ CCI_REG8(0x2a), 0x00 },

{ CCI_REG8(0x30), 0x50 },
{ CCI_REG8(0x31), 0x18 },
{ CCI_REG8(0x32), 0x32 },
{ CCI_REG8(0x33), 0x40 },
{ CCI_REG8(0x34), 0x50 },
{ CCI_REG8(0x35), 0x70 },
{ CCI_REG8(0x36), 0xa0 },

/* Out2 th */
{ CCI_REG8(0x40), 0xa0 },
{ CCI_REG8(0x41), 0x40 },
{ CCI_REG8(0x42), 0xa0 },
{ CCI_REG8(0x43), 0x90 },
{ CCI_REG8(0x44), 0x94 },
{ CCI_REG8(0x45), 0x84 },

/* Out1 th */
{ CCI_REG8(0x46), 0xb0 },
{ CCI_REG8(0x47), 0x55 },
{ CCI_REG8(0x48), 0xb0 },
{ CCI_REG8(0x49), 0xb0 },
{ CCI_REG8(0x4a), 0x94 },
{ CCI_REG8(0x4b), 0x84 },

/* Indoor th */
{ CCI_REG8(0x4c), 0xb0 },
{ CCI_REG8(0x4d), 0x40 },
{ CCI_REG8(0x4e), 0x90 },
{ CCI_REG8(0x4f), 0x90 },
{ CCI_REG8(0x50), 0x90 },
{ CCI_REG8(0x51), 0x80 },

/* Dark1 th */
{ CCI_REG8(0x52), 0xb0 },
{ CCI_REG8(0x53), 0x50 },
{ CCI_REG8(0x54), 0xb0 },
{ CCI_REG8(0x55), 0xb0 },
{ CCI_REG8(0x56), 0xb0 },
{ CCI_REG8(0x57), 0x7b },

/* Dark2 th */
{ CCI_REG8(0x58), 0xa0 },
{ CCI_REG8(0x59), 0x40 },
{ CCI_REG8(0x5a), 0xc0 },
{ CCI_REG8(0x5b), 0xc0 },
{ CCI_REG8(0x5c), 0xc8 },
{ CCI_REG8(0x5d), 0x7b },

/* Dark3 th */
{ CCI_REG8(0x5e), 0x9c },
{ CCI_REG8(0x5f), 0x40 },
{ CCI_REG8(0x60), 0xc8 },
{ CCI_REG8(0x61), 0xc8 },
{ CCI_REG8(0x62), 0xc8 },
{ CCI_REG8(0x63), 0x7b },

{ CCI_REG8(0x70), 0x15 },
{ CCI_REG8(0x71), 0x01 }, /* Don't Touch register */

{ CCI_REG8(0x72), 0x18 },
{ CCI_REG8(0x73), 0x01 }, /* Don't Touch register */

{ CCI_REG8(0x74), 0x25 },
{ CCI_REG8(0x75), 0x15 },

{ CCI_REG8(0x80), 0x20 },
{ CCI_REG8(0x81), 0x40 },
{ CCI_REG8(0x82), 0x65 },
{ CCI_REG8(0x85), 0x1a },
{ CCI_REG8(0x88), 0x00 },
{ CCI_REG8(0x89), 0x00 },
{ CCI_REG8(0x90), 0x5d }, /* add 20120430 */

/* Dont Touch register */
{ CCI_REG8(0xD0), 0x0c },
{ CCI_REG8(0xD1), 0x80 },
{ CCI_REG8(0xD2), 0x17 },
{ CCI_REG8(0xD3), 0x00 },
{ CCI_REG8(0xD4), 0x00 },
{ CCI_REG8(0xd5), 0x0f },
{ CCI_REG8(0xD6), 0xff },
{ CCI_REG8(0xd7), 0xff },
/* End */

{ CCI_REG8(0x3b), 0x06 },
{ CCI_REG8(0x3c), 0x06 },

/* Don't Touch register */
{ CCI_REG8(0xc5), 0x30 }, /* 55 -> 48 */
{ CCI_REG8(0xc6), 0x2a }, /* 48 -> 40 */
/* PAGE 12 END */

/* PAGE 13 START */
{ CCI_REG8(0x03), 0x13 },
{ CCI_REG8(0x10), 0xcb },
{ CCI_REG8(0x11), 0x7b },
{ CCI_REG8(0x12), 0x07 },
{ CCI_REG8(0x14), 0x00 },

{ CCI_REG8(0x20), 0x15 },
{ CCI_REG8(0x21), 0x13 },
{ CCI_REG8(0x22), 0x33 },
{ CCI_REG8(0x23), 0x05 },
{ CCI_REG8(0x24), 0x09 },

{ CCI_REG8(0x25), 0x0a },

{ CCI_REG8(0x26), 0x18 },
{ CCI_REG8(0x27), 0x30 },
{ CCI_REG8(0x29), 0x12 },
{ CCI_REG8(0x2a), 0x50 },

/* Low clip th */
{ CCI_REG8(0x2b), 0x02 },
{ CCI_REG8(0x2c), 0x02 },
{ CCI_REG8(0x25), 0x06 },
{ CCI_REG8(0x2d), 0x0c },
{ CCI_REG8(0x2e), 0x12 },
{ CCI_REG8(0x2f), 0x12 },

/* Out2 Edge */
{ CCI_REG8(0x50), 0x10 },
{ CCI_REG8(0x51), 0x14 },
{ CCI_REG8(0x52), 0x12 },
{ CCI_REG8(0x53), 0x0c },
{ CCI_REG8(0x54), 0x0f },
{ CCI_REG8(0x55), 0x0c },

/* Out1 Edge */
{ CCI_REG8(0x56), 0x0f },
{ CCI_REG8(0x57), 0x12 },
{ CCI_REG8(0x58), 0x12 },
{ CCI_REG8(0x59), 0x09 },
{ CCI_REG8(0x5a), 0x0c },
{ CCI_REG8(0x5b), 0x0c },

/* Indoor Edge */
{ CCI_REG8(0x5c), 0x0a },
{ CCI_REG8(0x5d), 0x0b },
{ CCI_REG8(0x5e), 0x0a },
{ CCI_REG8(0x5f), 0x08 },
{ CCI_REG8(0x60), 0x09 },
{ CCI_REG8(0x61), 0x08 },

/* Dark1 Edge */
{ CCI_REG8(0x62), 0x09 },
{ CCI_REG8(0x63), 0x09 },
{ CCI_REG8(0x64), 0x09 },
{ CCI_REG8(0x65), 0x07 },
{ CCI_REG8(0x66), 0x07 },
{ CCI_REG8(0x67), 0x07 },

/* Dark2 Edge */
{ CCI_REG8(0x68), 0x08 },
{ CCI_REG8(0x69), 0x08 },
{ CCI_REG8(0x6a), 0x08 },
{ CCI_REG8(0x6b), 0x06 },
{ CCI_REG8(0x6c), 0x06 },
{ CCI_REG8(0x6d), 0x06 },

/* Dark3 Edge */
{ CCI_REG8(0x6e), 0x08 },
{ CCI_REG8(0x6f), 0x08 },
{ CCI_REG8(0x70), 0x08 },
{ CCI_REG8(0x71), 0x06 },
{ CCI_REG8(0x72), 0x06 },
{ CCI_REG8(0x73), 0x06 },

/* 2DY */
{ CCI_REG8(0x80), 0x00 },
{ CCI_REG8(0x81), 0x1f },
{ CCI_REG8(0x82), 0x05 },
{ CCI_REG8(0x83), 0x31 },

{ CCI_REG8(0x90), 0x05 },
{ CCI_REG8(0x91), 0x05 },
{ CCI_REG8(0x92), 0x33 },
{ CCI_REG8(0x93), 0x30 },
{ CCI_REG8(0x94), 0x03 },
{ CCI_REG8(0x95), 0x14 },
{ CCI_REG8(0x97), 0x20 },
{ CCI_REG8(0x99), 0x20 },

{ CCI_REG8(0xa0), 0x01 },
{ CCI_REG8(0xa1), 0x02 },
{ CCI_REG8(0xa2), 0x01 },
{ CCI_REG8(0xa3), 0x02 },
{ CCI_REG8(0xa4), 0x05 },
{ CCI_REG8(0xa5), 0x05 },
{ CCI_REG8(0xa6), 0x07 },
{ CCI_REG8(0xa7), 0x08 },
{ CCI_REG8(0xa8), 0x07 },
{ CCI_REG8(0xa9), 0x08 },
{ CCI_REG8(0xaa), 0x07 },
{ CCI_REG8(0xab), 0x08 },

/* Out2 */
{ CCI_REG8(0xb0), 0x22 },
{ CCI_REG8(0xb1), 0x2a },
{ CCI_REG8(0xb2), 0x28 },
{ CCI_REG8(0xb3), 0x22 },
{ CCI_REG8(0xb4), 0x2a },
{ CCI_REG8(0xb5), 0x28 },

/* Out1 */
{ CCI_REG8(0xb6), 0x22 },
{ CCI_REG8(0xb7), 0x2a },
{ CCI_REG8(0xb8), 0x28 },
{ CCI_REG8(0xb9), 0x22 },
{ CCI_REG8(0xba), 0x2a },
{ CCI_REG8(0xbb), 0x28 },

/* Indoor */
{ CCI_REG8(0xbc), 0x25 },
{ CCI_REG8(0xbd), 0x2a },
{ CCI_REG8(0xbe), 0x27 },
{ CCI_REG8(0xbf), 0x25 },
{ CCI_REG8(0xc0), 0x2a },
{ CCI_REG8(0xc1), 0x27 },

/* Dark1 */
{ CCI_REG8(0xc2), 0x1e },
{ CCI_REG8(0xc3), 0x24 },
{ CCI_REG8(0xc4), 0x20 },
{ CCI_REG8(0xc5), 0x1e },
{ CCI_REG8(0xc6), 0x24 },
{ CCI_REG8(0xc7), 0x20 },

/*Dark2*/
{ CCI_REG8(0xc8), 0x18 },
{ CCI_REG8(0xc9), 0x20 },
{ CCI_REG8(0xca), 0x1e },
{ CCI_REG8(0xcb), 0x18 },
{ CCI_REG8(0xcc), 0x20 },
{ CCI_REG8(0xcd), 0x1e },

/* Dark3 */
{ CCI_REG8(0xce), 0x18 },
{ CCI_REG8(0xcf), 0x20 },
{ CCI_REG8(0xd0), 0x1e },
{ CCI_REG8(0xd1), 0x18 },
{ CCI_REG8(0xd2), 0x20 },
{ CCI_REG8(0xd3), 0x1e },
/* PAGE 13 END */

/* PAGE 14 START */
{ CCI_REG8(0x03), 0x14 },
{ CCI_REG8(0x10), 0x01 },

{ CCI_REG8(0x14), 0xb0 }, /* GX */
{ CCI_REG8(0x15), 0x90 }, /* GY */
{ CCI_REG8(0x16), 0xa0 }, /* RX */
{ CCI_REG8(0x17), 0x80 }, /* RY */
{ CCI_REG8(0x18), 0xa0 }, /* BX */
{ CCI_REG8(0x19), 0x80 }, /* BY */

{ CCI_REG8(0x20), 0xa0 }, /* X */
{ CCI_REG8(0x21), 0x80 }, /* Y */

{ CCI_REG8(0x22), 0x80 },
{ CCI_REG8(0x23), 0x80 },
{ CCI_REG8(0x24), 0x80 },

{ CCI_REG8(0x30), 0xc8 },
{ CCI_REG8(0x31), 0x2b },
{ CCI_REG8(0x32), 0x00 },
{ CCI_REG8(0x33), 0x00 },
{ CCI_REG8(0x34), 0x90 },

{ CCI_REG8(0x40), 0x50 }, // 3e //
{ CCI_REG8(0x50), 0x38 }, // 28 //
{ CCI_REG8(0x60), 0x34 }, // 24 //
{ CCI_REG8(0x70), 0x38 }, // 28 //
/* PAGE 14 END */

/* PAGE 15 START */
{ CCI_REG8(0x03), 0x15 },
{ CCI_REG8(0x10), 0x0f },

/* Rstep H 16 */
/* Rstep L 14 */
{ CCI_REG8(0x14), 0x46 }, /* CMCOFSGH */
{ CCI_REG8(0x15), 0x38 }, /* CMCOFSGM */
{ CCI_REG8(0x16), 0x28 }, /* CMCOFSGL */
{ CCI_REG8(0x17), 0x2f }, /* CMC SIGN */

/* CMC */
{ CCI_REG8(0x30), 0x7e },
{ CCI_REG8(0x31), 0x43 },
{ CCI_REG8(0x32), 0x05 },
{ CCI_REG8(0x33), 0x20 },
{ CCI_REG8(0x34), 0x65 },
{ CCI_REG8(0x35), 0x05 },
{ CCI_REG8(0x36), 0x0c },
{ CCI_REG8(0x37), 0x3e },
{ CCI_REG8(0x38), 0x8a },

/* CMC OFS */
{ CCI_REG8(0x40), 0x84 },
{ CCI_REG8(0x41), 0x0b },
{ CCI_REG8(0x42), 0x84 },
{ CCI_REG8(0x43), 0x08 },
{ CCI_REG8(0x44), 0x87 },
{ CCI_REG8(0x45), 0x00 },
{ CCI_REG8(0x46), 0x82 },
{ CCI_REG8(0x47), 0x9e },
{ CCI_REG8(0x48), 0x24 },

/* CMC POFS*/
{ CCI_REG8(0x50), 0x0b },
{ CCI_REG8(0x51), 0x8a },
{ CCI_REG8(0x52), 0x00 },
{ CCI_REG8(0x53), 0x0e },
{ CCI_REG8(0x54), 0x03 },
{ CCI_REG8(0x55), 0x92 },
{ CCI_REG8(0x56), 0x05 },
{ CCI_REG8(0x57), 0x92 },
{ CCI_REG8(0x58), 0x0c },

{ CCI_REG8(0x80), 0x00 },
{ CCI_REG8(0x85), 0x80 },
{ CCI_REG8(0x87), 0x02 },
{ CCI_REG8(0x88), 0x00 },
{ CCI_REG8(0x89), 0x00 },
{ CCI_REG8(0x8a), 0x00 },
/* PAGE 15 END */

/* PAGE 16 START */
{ CCI_REG8(0x03), 0x16 },
{ CCI_REG8(0x10), 0x31 },
{ CCI_REG8(0x18), 0x5e },/* Double_AG 5e->37 */
{ CCI_REG8(0x19), 0x5d },/* Double_AG 5e->36 */
{ CCI_REG8(0x1a), 0x0e },
{ CCI_REG8(0x1b), 0x01 },
{ CCI_REG8(0x1c), 0xdc },
{ CCI_REG8(0x1d), 0xfe },

/* GMA Default */
{ CCI_REG8(0x30), 0x00 },
{ CCI_REG8(0x31), 0x0e },
{ CCI_REG8(0x32), 0x1e },
{ CCI_REG8(0x33), 0x34 },
{ CCI_REG8(0x34), 0x56 },
{ CCI_REG8(0x35), 0x74 },
{ CCI_REG8(0x36), 0x8b },
{ CCI_REG8(0x37), 0x9c },
{ CCI_REG8(0x38), 0xaa },
{ CCI_REG8(0x39), 0xbc },
{ CCI_REG8(0x3a), 0xc9 },
{ CCI_REG8(0x3b), 0xd5 },
{ CCI_REG8(0x3c), 0xdf },
{ CCI_REG8(0x3d), 0xe7 },
{ CCI_REG8(0x3e), 0xef },
{ CCI_REG8(0x3f), 0xf4 },
{ CCI_REG8(0x40), 0xf8 },
{ CCI_REG8(0x41), 0xfd },
{ CCI_REG8(0x42), 0xff },

{ CCI_REG8(0x50), 0x00 },
{ CCI_REG8(0x51), 0x08 },
{ CCI_REG8(0x52), 0x1e },
{ CCI_REG8(0x53), 0x36 },
{ CCI_REG8(0x54), 0x5a },
{ CCI_REG8(0x55), 0x75 },
{ CCI_REG8(0x56), 0x8d },
{ CCI_REG8(0x57), 0xa1 },
{ CCI_REG8(0x58), 0xb2 },
{ CCI_REG8(0x59), 0xbe },
{ CCI_REG8(0x5a), 0xc9 },
{ CCI_REG8(0x5b), 0xd2 },
{ CCI_REG8(0x5c), 0xdb },
{ CCI_REG8(0x5d), 0xe3 },
{ CCI_REG8(0x5e), 0xeb },
{ CCI_REG8(0x5f), 0xf0 },
{ CCI_REG8(0x60), 0xf5 },
{ CCI_REG8(0x61), 0xf7 },
{ CCI_REG8(0x62), 0xf8 },

{ CCI_REG8(0x70), 0x00 },
{ CCI_REG8(0x71), 0x0e },
{ CCI_REG8(0x72), 0x1f },
{ CCI_REG8(0x73), 0x3f },
{ CCI_REG8(0x74), 0x5d },
{ CCI_REG8(0x75), 0x75 },
{ CCI_REG8(0x76), 0x8a },
{ CCI_REG8(0x77), 0x9c },
{ CCI_REG8(0x78), 0xad },
{ CCI_REG8(0x79), 0xbb },
{ CCI_REG8(0x7a), 0xc6 },
{ CCI_REG8(0x7b), 0xd1 },
{ CCI_REG8(0x7c), 0xda },
{ CCI_REG8(0x7d), 0xe3 },
{ CCI_REG8(0x7e), 0xea },
{ CCI_REG8(0x7f), 0xf1 },
{ CCI_REG8(0x80), 0xf6 },
{ CCI_REG8(0x81), 0xfb },
{ CCI_REG8(0x82), 0xff },
/* PAGE 16 END */

/* PAGE 17 START */
{ CCI_REG8(0x03), 0x17 },
{ CCI_REG8(0x10), 0xf7 },
/* PAGE 17 END */

/* scaler off */
{ CCI_REG8(0x03), 0x18 },
{ CCI_REG8(0x10), 0x00 },

/* PAGE 20 START */
{ CCI_REG8(0x03), 0x20 },
{ CCI_REG8(0x11), 0x1c },
{ CCI_REG8(0x18), 0x30 },
{ CCI_REG8(0x1a), 0x08 },
{ CCI_REG8(0x20), 0x05 },
{ CCI_REG8(0x21), 0x30 },
{ CCI_REG8(0x22), 0x10 },
{ CCI_REG8(0x23), 0x00 },
{ CCI_REG8(0x24), 0x00 },

{ CCI_REG8(0x28), 0xe7 },
{ CCI_REG8(0x29), 0x0d }, /* 20100305 ad->0d */
{ CCI_REG8(0x2a), 0xff },
{ CCI_REG8(0x2b), 0x34 },

{ CCI_REG8(0x2c), 0xc2 },
{ CCI_REG8(0x2d), 0x5f },
{ CCI_REG8(0x2e), 0x33 },
{ CCI_REG8(0x30), 0x78 },
{ CCI_REG8(0x32), 0x03 },
{ CCI_REG8(0x33), 0x2e },
{ CCI_REG8(0x34), 0x30 },
{ CCI_REG8(0x35), 0xd4 },
{ CCI_REG8(0x36), 0xfe },
{ CCI_REG8(0x37), 0x32 },
{ CCI_REG8(0x38), 0x04 },
{ CCI_REG8(0x39), 0x22 },
{ CCI_REG8(0x3a), 0xde },
{ CCI_REG8(0x3b), 0x22 },
{ CCI_REG8(0x3c), 0xde },

{ CCI_REG8(0x50), 0x45 },
{ CCI_REG8(0x51), 0x88 },

{ CCI_REG8(0x56), 0x27 },
{ CCI_REG8(0x57), 0xa0 },
{ CCI_REG8(0x58), 0x20 },
{ CCI_REG8(0x59), 0x74 },
{ CCI_REG8(0x5a), 0x04 },

{ CCI_REG8(0x60), 0x55 },
{ CCI_REG8(0x61), 0x55 },
{ CCI_REG8(0x62), 0x6A },
{ CCI_REG8(0x63), 0xA9 },
{ CCI_REG8(0x64), 0x6A },
{ CCI_REG8(0x65), 0xA9 },
{ CCI_REG8(0x66), 0x6B },
{ CCI_REG8(0x67), 0xE9 },
{ CCI_REG8(0x68), 0x6B },
{ CCI_REG8(0x69), 0xE9 },
{ CCI_REG8(0x6a), 0x6A },
{ CCI_REG8(0x6b), 0xA9 },
{ CCI_REG8(0x6c), 0x6A },
{ CCI_REG8(0x6d), 0xA9 },
{ CCI_REG8(0x6e), 0x55 },
{ CCI_REG8(0x6f), 0x55 },

{ CCI_REG8(0x70), 0x73 }, /* 6c */
{ CCI_REG8(0x71), 0x80 }, /* 82(+8) */

{ CCI_REG8(0x76), 0x43 },
{ CCI_REG8(0x77), 0xf2 },
{ CCI_REG8(0x78), 0x23 }, /* 24 */
{ CCI_REG8(0x79), 0x45 }, /* Y Target 70 => 25, 72 => 26 */
{ CCI_REG8(0x7a), 0x23 }, /* 23 */
{ CCI_REG8(0x7b), 0x22 }, /* 22 */
{ CCI_REG8(0x7d), 0x23 },

{ CCI_REG8(0x83), 0x01 }, //EXP Normal 30.00 fps
{ CCI_REG8(0x84), 0xa5 },
{ CCI_REG8(0x85), 0xe0 },

{ CCI_REG8(0x86), 0x01 }, //EXPMin 6500.00 fps
{ CCI_REG8(0x87), 0xf4 },

{ CCI_REG8(0x88), 0x06 }, //EXP Max 8.00 fps
{ CCI_REG8(0x89), 0x2e },
{ CCI_REG8(0x8a), 0x08 },

{ CCI_REG8(0x8B), 0x7e }, //EXP100
{ CCI_REG8(0x8C), 0xf4 },

{ CCI_REG8(0x8D), 0x69 }, //EXP120
{ CCI_REG8(0x8E), 0x78 },

{ CCI_REG8(0x98), 0x9d },
{ CCI_REG8(0x99), 0x45 },
{ CCI_REG8(0x9a), 0x0d },
{ CCI_REG8(0x9b), 0xde },

{ CCI_REG8(0x9c), 0x17 }, //EXP Limit 541.67 fps
{ CCI_REG8(0x9d), 0x70 },

{ CCI_REG8(0x9e), 0x01 }, /*EXP Unit */
{ CCI_REG8(0x9f), 0xf4 },

{ CCI_REG8(0xb0), 0x18 },
{ CCI_REG8(0xb1), 0x14 },
{ CCI_REG8(0xb2), 0xb0 },
{ CCI_REG8(0xb3), 0x14 },
{ CCI_REG8(0xb4), 0x14 },
{ CCI_REG8(0xb5), 0x38 },
{ CCI_REG8(0xb6), 0x26 },
{ CCI_REG8(0xb7), 0x20 },
{ CCI_REG8(0xb8), 0x1d },
{ CCI_REG8(0xb9), 0x1b },
{ CCI_REG8(0xba), 0x1a },
{ CCI_REG8(0xbb), 0x19 },
{ CCI_REG8(0xbc), 0x46 },
{ CCI_REG8(0xbd), 0x44 },

{ CCI_REG8(0xc0), 0x10 },
{ CCI_REG8(0xc1), 0x3c },
{ CCI_REG8(0xc2), 0x3c },
{ CCI_REG8(0xc3), 0x3c },
{ CCI_REG8(0xc4), 0x08 },

{ CCI_REG8(0xc8), 0x80 },
{ CCI_REG8(0xc9), 0x80 },
/* PAGE 20 END */

/* PAGE 22 START */
{ CCI_REG8(0x03), 0x22 },
{ CCI_REG8(0x10), 0xfd },
{ CCI_REG8(0x11), 0x2e },
{ CCI_REG8(0x19), 0x01 }, /* Low On */
{ CCI_REG8(0x20), 0x30 },
{ CCI_REG8(0x21), 0x40 },
{ CCI_REG8(0x24), 0x01 },
{ CCI_REG8(0x25), 0x7e }, /* Add 20120514 light stable */

{ CCI_REG8(0x30), 0x80 },
{ CCI_REG8(0x31), 0x81 },
{ CCI_REG8(0x38), 0x11 },
{ CCI_REG8(0x39), 0x34 },
{ CCI_REG8(0x40), 0xe4 },

{ CCI_REG8(0x41), 0x43 }, /* 33 */
{ CCI_REG8(0x42), 0x22 }, /* 22 */
{ CCI_REG8(0x43), 0xf1 }, /* f6 */
{ CCI_REG8(0x44), 0x54 }, /* 44 */
{ CCI_REG8(0x45), 0x22 }, /* 33 */
{ CCI_REG8(0x46), 0x02 },
{ CCI_REG8(0x50), 0xb2 },
{ CCI_REG8(0x51), 0x81 },
{ CCI_REG8(0x52), 0x98 },

{ CCI_REG8(0x80), 0x38 },
{ CCI_REG8(0x81), 0x20 },
{ CCI_REG8(0x82), 0x3a }, /* 3a */

{ CCI_REG8(0x83), 0x5b },
{ CCI_REG8(0x84), 0x2a },
{ CCI_REG8(0x85), 0x53 },
{ CCI_REG8(0x86), 0x20 }, // B Min D65

{ CCI_REG8(0x87), 0x42 }, //43
{ CCI_REG8(0x88), 0x35 }, //31
{ CCI_REG8(0x89), 0x3d },
{ CCI_REG8(0x8a), 0x2a }, //29

{ CCI_REG8(0x8b), 0x3f }, //40
{ CCI_REG8(0x8c), 0x36 }, //37
{ CCI_REG8(0x8d), 0x3a }, //3a
{ CCI_REG8(0x8e), 0x33 },

{ CCI_REG8(0x8f), 0x5c },
{ CCI_REG8(0x90), 0x5b },
{ CCI_REG8(0x91), 0x57 },
{ CCI_REG8(0x92), 0x4f },
{ CCI_REG8(0x93), 0x41 },
{ CCI_REG8(0x94), 0x3a },
{ CCI_REG8(0x95), 0x32 },
{ CCI_REG8(0x96), 0x2b },
{ CCI_REG8(0x97), 0x23 },
{ CCI_REG8(0x98), 0x20 },
{ CCI_REG8(0x99), 0x1f },
{ CCI_REG8(0x9a), 0x1f },

{ CCI_REG8(0x9b), 0x78 },
{ CCI_REG8(0x9c), 0x77 },
{ CCI_REG8(0x9d), 0x48 },
{ CCI_REG8(0x9e), 0x37 },
{ CCI_REG8(0x9f), 0x30 },

{ CCI_REG8(0xa0), 0xf0 },
{ CCI_REG8(0xa1), 0x44 },
{ CCI_REG8(0xa2), 0xff },
{ CCI_REG8(0xa3), 0xff },

{ CCI_REG8(0xa4), 0x09 }, /* 1500fps */
{ CCI_REG8(0xa5), 0x2c }, /* 700fps */
{ CCI_REG8(0xa6), 0xbd }, /*20150119 cf -> bd*/

{ CCI_REG8(0xad), 0x40 },
{ CCI_REG8(0xae), 0x4a },

{ CCI_REG8(0xaf), 0x2f },  /* low temp Rgain */
{ CCI_REG8(0xb0), 0x2d },  /* low temp Rgain */

{ CCI_REG8(0xb1), 0x00 }, /* 0x20 -> 0x00 0405 modify */
{ CCI_REG8(0xb4), 0xbf },
{ CCI_REG8(0xb8), 0xc1 }, /* a2:b-2,R+2  b4:B-3,R+4 lowtemp b0 a1 Spec AWB A modify */
{ CCI_REG8(0xb9), 0x00 },
/* PAGE 22 END */

/* PAGE 48 START */
{ CCI_REG8(0x03), 0x48 },

/* PLL Setting */
{ CCI_REG8(0x70), 0x05 },
{ CCI_REG8(0x71), 0x30 }, /* MiPi Pllx2 */
{ CCI_REG8(0x72), 0x85 },
{ CCI_REG8(0x70), 0xa5 }, /* PLL Enable */
{ CCI_REG8(0x03), 0x48 },
{ CCI_REG8(0x03), 0x48 },
{ CCI_REG8(0x03), 0x48 },
{ CCI_REG8(0x03), 0x48 },
{ CCI_REG8(0x70), 0x95 }, /* CLK_GEN_ENABLE */

/* MIPI TX Setting */
{ CCI_REG8(0x11), 0x00 }, /* 20111013 0x10 continuous -> 0x00 not Continuous */
{ CCI_REG8(0x10), 0x1c },
{ CCI_REG8(0x12), 0x00 },
{ CCI_REG8(0x14), 0x30 }, /*0x1470 */ /* 201110130x00 -> 0x30 Clock Delay */
{ CCI_REG8(0x16), 0x04 }, /*1016  0x04 -> 0x05*/

{ CCI_REG8(0x19), 0x00 },
{ CCI_REG8(0x1a), 0x30 },
{ CCI_REG8(0x1b), 0x17 },
{ CCI_REG8(0x1c), 0x0c },
{ CCI_REG8(0x1d), 0x10 },
{ CCI_REG8(0x1e), 0x06 },
{ CCI_REG8(0x1f), 0x03 }, /* 0x05->0x03 20131101 */ /* 0x03->0x02 20140509 */
{ CCI_REG8(0x20), 0x00 },

{ CCI_REG8(0x23), 0x01 },
{ CCI_REG8(0x24), 0x1e },
{ CCI_REG8(0x25), 0x00 },
{ CCI_REG8(0x26), 0x00 },
{ CCI_REG8(0x27), 0x01 },
{ CCI_REG8(0x28), 0x00 },
{ CCI_REG8(0x2a), 0x06 },
{ CCI_REG8(0x2b), 0x40 },
{ CCI_REG8(0x2c), 0x04 },
{ CCI_REG8(0x2d), 0xb0 },

{ CCI_REG8(0x30), 0x40 }, /* 800x600 MiPi OutPut */
{ CCI_REG8(0x31), 0x06 },

{ CCI_REG8(0x32), 0x0c },
{ CCI_REG8(0x33), 0x0a },
{ CCI_REG8(0x34), 0x01 }, /* CLK LP -> HS Prepare time 24MHz:0x02, 48MHz:0x03 */
{ CCI_REG8(0x35), 0x03 },
{ CCI_REG8(0x36), 0x01 },
{ CCI_REG8(0x37), 0x07 },
{ CCI_REG8(0x38), 0x02 },
{ CCI_REG8(0x39), 0x02 }, /* drivability 24MHZ: 0x02, 48MHz:0x03 */
/* {0x17, 0xc4,}, */ /* MHSHIM */
/* {0x17, 0xc0,}, */ /* MHSHIM */
/* {0x17, 0x00,}, */ /* MHSHIM */
{ CCI_REG8(0x50), 0x00 },
/* PAGE 48 END */

/* PAGE 20 */
{ CCI_REG8(0x03), 0x20 },
{ CCI_REG8(0x10), 0x8c }, /* AE on 60hz */

/* PAGE 22 */
{ CCI_REG8(0x03), 0x22 },
{ CCI_REG8(0x10), 0xe9 },

/* PAGE 0 */
{ CCI_REG8(0x03), 0x00 },
{ CCI_REG8(0x03), 0x00 },
{ CCI_REG8(0x03), 0x00 },
{ CCI_REG8(0x03), 0x00 },
{ CCI_REG8(0x03), 0x00 },
{ CCI_REG8(0x03), 0x00 },
{ CCI_REG8(0x03), 0x00 },
{ CCI_REG8(0x03), 0x00 },
{ CCI_REG8(0x03), 0x00 },
{ CCI_REG8(0x03), 0x00 },
{ CCI_REG8(0x03), 0x00 },
{ CCI_REG8(0x03), 0x00 },
{ CCI_REG8(0x03), 0x00 },
{ CCI_REG8(0x03), 0x00 },
{ CCI_REG8(0x03), 0x00 },
{ CCI_REG8(0x03), 0x00 },
{ CCI_REG8(0x03), 0x00 },
{ CCI_REG8(0x03), 0x00 },
{ CCI_REG8(0x03), 0x00 },
{ CCI_REG8(0x03), 0x00 },

{ CCI_REG8(0x03), 0x00 },

{ CCI_REG8(0x01), 0x30 },

};

static void sr200pc20_fill_format(struct v4l2_mbus_framefmt *fmt)
{
	fmt->width = SR200PC20_WIDTH;
	fmt->height = SR200PC20_HEIGHT;
	fmt->code = MEDIA_BUS_FMT_UYVY8_1X16;
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_SRGB;
	fmt->ycbcr_enc = V4L2_YCBCR_ENC_601;
	fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	fmt->xfer_func = V4L2_XFER_FUNC_SRGB;
}

static int sr200pc20_enum_mbus_code(struct v4l2_subdev *sd,
				    struct v4l2_subdev_state *state,
				    struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index)
		return -EINVAL;

	code->code = MEDIA_BUS_FMT_UYVY8_1X16;
	return 0;
}

static int sr200pc20_enum_frame_size(struct v4l2_subdev *sd,
				     struct v4l2_subdev_state *state,
				     struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index || fse->code != MEDIA_BUS_FMT_UYVY8_1X16)
		return -EINVAL;

	fse->min_width = SR200PC20_WIDTH;
	fse->max_width = SR200PC20_WIDTH;
	fse->min_height = SR200PC20_HEIGHT;
	fse->max_height = SR200PC20_HEIGHT;
	return 0;
}

static int sr200pc20_set_format(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *state,
				struct v4l2_subdev_format *format)
{
	struct v4l2_mbus_framefmt *fmt;

	sr200pc20_fill_format(&format->format);
	fmt = v4l2_subdev_state_get_format(state, format->pad);
	*fmt = format->format;

	return 0;
}

static int sr200pc20_get_selection(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *state,
				   struct v4l2_subdev_selection *sel)
{
	if (sel->pad)
		return -EINVAL;

	switch (sel->target) {
	case V4L2_SEL_TGT_NATIVE_SIZE:
	case V4L2_SEL_TGT_CROP_BOUNDS:
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP:
		sel->r = (struct v4l2_rect) {
			.width = SR200PC20_WIDTH,
			.height = SR200PC20_HEIGHT,
		};
		return 0;
	default:
		return -EINVAL;
	}
}

static int sr200pc20_init_state(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *state)
{
	struct v4l2_subdev_format format = {
		.pad = 0,
		.which = V4L2_SUBDEV_FORMAT_TRY,
	};

	return sr200pc20_set_format(sd, state, &format);
}

static int sr200pc20_start_streaming(struct sr200pc20 *sensor)
{
	struct device *dev = sensor->sd.dev;
	int ret = 0;

	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0)
		return ret;

	cci_multi_reg_write(sensor->regmap, sr200pc20_800x600_60hz,
			    ARRAY_SIZE(sr200pc20_800x600_60hz), &ret);
	if (ret) {
		dev_err(dev, "failed to program preview mode: %d\n", ret);
		pm_runtime_put(dev);
		return ret;
	}

	/* The downstream 0xff/0x0a pseudo-register is a 100 ms delay. */
	msleep(100);
	return 0;
}

static void sr200pc20_stop_streaming(struct sr200pc20 *sensor)
{
	struct device *dev = sensor->sd.dev;
	int ret = 0;

	cci_write(sensor->regmap, SR200PC20_PAGE_SELECT, 0x00, &ret);
	cci_write(sensor->regmap, SR200PC20_SLEEP, 0x01, &ret);
	if (ret)
		dev_warn(dev, "failed to stop streaming: %d\n", ret);

	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);
}

static int sr200pc20_set_stream(struct v4l2_subdev *sd, int enable)
{
	struct sr200pc20 *sensor = to_sr200pc20(sd);
	struct v4l2_subdev_state *state;
	int ret = 0;

	state = v4l2_subdev_lock_and_get_active_state(sd);
	if (!!enable == sensor->streaming)
		goto unlock;

	if (enable)
		ret = sr200pc20_start_streaming(sensor);
	else
		sr200pc20_stop_streaming(sensor);

	if (!ret)
		sensor->streaming = enable;

unlock:
	v4l2_subdev_unlock_state(state);
	return ret;
}

static const struct v4l2_subdev_video_ops sr200pc20_video_ops = {
	.s_stream = sr200pc20_set_stream,
};

static const struct v4l2_subdev_pad_ops sr200pc20_pad_ops = {
	.enum_mbus_code = sr200pc20_enum_mbus_code,
	.enum_frame_size = sr200pc20_enum_frame_size,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = sr200pc20_set_format,
	.get_selection = sr200pc20_get_selection,
};

static const struct v4l2_subdev_ops sr200pc20_subdev_ops = {
	.video = &sr200pc20_video_ops,
	.pad = &sr200pc20_pad_ops,
};

static const struct v4l2_subdev_internal_ops sr200pc20_internal_ops = {
	.init_state = sr200pc20_init_state,
};

static int sr200pc20_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct sr200pc20 *sensor = to_sr200pc20(sd);
	int ret;

	gpiod_set_value_cansleep(sensor->standby_gpio, 0);
	usleep_range(1000, 1500);
	gpiod_set_value_cansleep(sensor->reset_gpio, 0);
	usleep_range(1000, 1500);

	ret = regulator_enable(sensor->vdig);
	if (ret)
		return ret;
	usleep_range(2000, 2500);

	ret = regulator_enable(sensor->vana);
	if (ret)
		goto disable_vdig;
	usleep_range(5000, 5500);

	ret = regulator_enable(sensor->vio);
	if (ret)
		goto disable_vana;
	usleep_range(3000, 3500);

	gpiod_set_value_cansleep(sensor->standby_gpio, 1);
	usleep_range(2000, 2500);

	ret = clk_prepare_enable(sensor->xclk);
	if (ret)
		goto standby_low;
	msleep(32);

	gpiod_set_value_cansleep(sensor->reset_gpio, 1);
	usleep_range(16000, 16500);
	return 0;

standby_low:
	gpiod_set_value_cansleep(sensor->standby_gpio, 0);
	regulator_disable(sensor->vio);
disable_vana:
	regulator_disable(sensor->vana);
disable_vdig:
	regulator_disable(sensor->vdig);
	return ret;
}

static int sr200pc20_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct sr200pc20 *sensor = to_sr200pc20(sd);

	gpiod_set_value_cansleep(sensor->reset_gpio, 0);
	usleep_range(16000, 16500);
	clk_disable_unprepare(sensor->xclk);
	gpiod_set_value_cansleep(sensor->standby_gpio, 0);
	usleep_range(3000, 3500);
	regulator_disable(sensor->vio);
	usleep_range(3000, 3500);
	regulator_disable(sensor->vana);
	usleep_range(3000, 3500);
	regulator_disable(sensor->vdig);

	return 0;
}

static int sr200pc20_identify(struct sr200pc20 *sensor)
{
	struct device *dev = sensor->sd.dev;
	u64 chip_id;
	int ret = 0;

	cci_write(sensor->regmap, SR200PC20_PAGE_SELECT, 0x00, &ret);
	cci_read(sensor->regmap, SR200PC20_CHIP_ID_REG, &chip_id, &ret);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read chip ID\n");

	if (chip_id != SR200PC20_CHIP_ID) {
		dev_err(dev, "chip ID mismatch: expected 0x%02x, got 0x%02llx\n",
			SR200PC20_CHIP_ID, chip_id);
		return -ENODEV;
	}

	dev_info(dev, "SR200PC20 detected (chip ID 0x%02llx)\n", chip_id);
	return 0;
}

static int sr200pc20_check_hwcfg(struct sr200pc20 *sensor)
{
	struct device *dev = sensor->sd.dev;
	struct v4l2_fwnode_endpoint ep = {
		.bus_type = V4L2_MBUS_CSI2_DPHY,
	};
	struct fwnode_handle *endpoint;
	int ret;

	endpoint = fwnode_graph_get_next_endpoint(dev_fwnode(dev), NULL);
	if (!endpoint)
		return dev_err_probe(dev, -EINVAL, "endpoint node not found\n");

	ret = v4l2_fwnode_endpoint_alloc_parse(endpoint, &ep);
	fwnode_handle_put(endpoint);
	if (ret)
		return ret;

	if (ep.bus.mipi_csi2.num_data_lanes != 1) {
		dev_err(dev, "exactly one CSI-2 data lane is required\n");
		ret = -EINVAL;
		goto free_endpoint;
	}

	if (ep.nr_of_link_frequencies != 1) {
		dev_err(dev, "one link-frequency value is required\n");
		ret = -EINVAL;
		goto free_endpoint;
	}

	sensor->link_freq = ep.link_frequencies[0];
	if (sensor->link_freq != SR200PC20_LINK_FREQ) {
		dev_err(dev, "unsupported link frequency %lld Hz\n",
			sensor->link_freq);
		ret = -EINVAL;
	}

free_endpoint:
	v4l2_fwnode_endpoint_free(&ep);
	return ret;
}

static int sr200pc20_init_controls(struct sr200pc20 *sensor)
{
	struct v4l2_fwnode_device_properties props;
	struct v4l2_ctrl *ctrl;
	int ret;

	ret = v4l2_ctrl_handler_init(&sensor->ctrls, 4);
	if (ret)
		return ret;

	ctrl = v4l2_ctrl_new_int_menu(&sensor->ctrls, NULL,
				      V4L2_CID_LINK_FREQ, 0, 0,
				      &sensor->link_freq);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	ctrl = v4l2_ctrl_new_std(&sensor->ctrls, NULL, V4L2_CID_PIXEL_RATE,
				 SR200PC20_PIXEL_RATE, SR200PC20_PIXEL_RATE,
				 1, SR200PC20_PIXEL_RATE);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	ret = v4l2_fwnode_device_parse(sensor->sd.dev, &props);
	if (ret)
		goto free_controls;

	ret = v4l2_ctrl_new_fwnode_properties(&sensor->ctrls, NULL, &props);
	if (ret)
		goto free_controls;

	if (sensor->ctrls.error) {
		ret = sensor->ctrls.error;
		goto free_controls;
	}

	sensor->sd.ctrl_handler = &sensor->ctrls;
	return 0;

free_controls:
	v4l2_ctrl_handler_free(&sensor->ctrls);
	return ret;
}

static int sr200pc20_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct sr200pc20 *sensor;
	unsigned long xclk_rate;
	int ret;

	sensor = devm_kzalloc(dev, sizeof(*sensor), GFP_KERNEL);
	if (!sensor)
		return -ENOMEM;

	v4l2_i2c_subdev_init(&sensor->sd, client, &sr200pc20_subdev_ops);
	sensor->sd.internal_ops = &sr200pc20_internal_ops;

	ret = sr200pc20_check_hwcfg(sensor);
	if (ret)
		return ret;

	sensor->xclk = devm_clk_get(dev, "xclk");
	if (IS_ERR(sensor->xclk))
		return dev_err_probe(dev, PTR_ERR(sensor->xclk),
				     "failed to get xclk\n");

	ret = clk_set_rate(sensor->xclk, SR200PC20_XCLK_FREQ);
	if (ret)
		return dev_err_probe(dev, ret, "failed to set xclk rate\n");

	xclk_rate = clk_get_rate(sensor->xclk);
	if (xclk_rate < SR200PC20_XCLK_MIN ||
	    xclk_rate > SR200PC20_XCLK_MAX)
		return dev_err_probe(dev, -EINVAL,
				     "unsupported xclk rate %lu Hz\n", xclk_rate);

	sensor->vdig = devm_regulator_get(dev, "vdig");
	if (IS_ERR(sensor->vdig))
		return dev_err_probe(dev, PTR_ERR(sensor->vdig),
				     "failed to get vdig supply\n");
	sensor->vana = devm_regulator_get(dev, "vana");
	if (IS_ERR(sensor->vana))
		return dev_err_probe(dev, PTR_ERR(sensor->vana),
				     "failed to get vana supply\n");
	sensor->vio = devm_regulator_get(dev, "vio");
	if (IS_ERR(sensor->vio))
		return dev_err_probe(dev, PTR_ERR(sensor->vio),
				     "failed to get vio supply\n");

	sensor->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(sensor->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(sensor->reset_gpio),
				     "failed to get reset GPIO\n");
	sensor->standby_gpio = devm_gpiod_get(dev, "standby", GPIOD_OUT_LOW);
	if (IS_ERR(sensor->standby_gpio))
		return dev_err_probe(dev, PTR_ERR(sensor->standby_gpio),
				     "failed to get standby GPIO\n");

	sensor->regmap = devm_cci_regmap_init_i2c(client, 8);
	if (IS_ERR(sensor->regmap))
		return dev_err_probe(dev, PTR_ERR(sensor->regmap),
				     "failed to initialize CCI regmap\n");

	ret = sr200pc20_power_on(dev);
	if (ret)
		return dev_err_probe(dev, ret, "failed to power on sensor\n");

	ret = sr200pc20_identify(sensor);
	if (ret)
		goto power_off;

	ret = sr200pc20_init_controls(sensor);
	if (ret)
		goto power_off;

	sensor->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	sensor->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	sensor->pad.flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&sensor->sd.entity, 1, &sensor->pad);
	if (ret)
		goto free_controls;

	sensor->sd.state_lock = sensor->ctrls.lock;
	ret = v4l2_subdev_init_finalize(&sensor->sd);
	if (ret)
		goto cleanup_entity;

	pm_runtime_set_active(dev);
	pm_runtime_get_noresume(dev);
	pm_runtime_enable(dev);
	pm_runtime_set_autosuspend_delay(dev, 1000);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_put_autosuspend(dev);

	ret = v4l2_async_register_subdev_sensor(&sensor->sd);
	if (ret)
		goto cleanup_subdev;

	return 0;

cleanup_subdev:
	pm_runtime_disable(dev);
	pm_runtime_set_suspended(dev);
	v4l2_subdev_cleanup(&sensor->sd);
cleanup_entity:
	media_entity_cleanup(&sensor->sd.entity);
free_controls:
	v4l2_ctrl_handler_free(&sensor->ctrls);
power_off:
	sr200pc20_power_off(dev);
	return ret;
}

static void sr200pc20_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct sr200pc20 *sensor = to_sr200pc20(sd);
	struct device *dev = &client->dev;

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&sensor->ctrls);

	pm_runtime_disable(dev);
	if (!pm_runtime_status_suspended(dev))
		sr200pc20_power_off(dev);
	pm_runtime_set_suspended(dev);
}

static const struct dev_pm_ops sr200pc20_pm_ops = {
	RUNTIME_PM_OPS(sr200pc20_power_off, sr200pc20_power_on, NULL)
};

static const struct of_device_id sr200pc20_of_match[] = {
	{ .compatible = "siliconfile,sr200pc20" },
	{ }
};
MODULE_DEVICE_TABLE(of, sr200pc20_of_match);

static struct i2c_driver sr200pc20_i2c_driver = {
	.driver = {
		.name = "sr200pc20",
		.of_match_table = sr200pc20_of_match,
		.pm = pm_ptr(&sr200pc20_pm_ops),
	},
	.probe = sr200pc20_probe,
	.remove = sr200pc20_remove,
};
module_i2c_driver(sr200pc20_i2c_driver);

MODULE_DESCRIPTION("SiliconFile SR200PC20 camera sensor driver");
MODULE_LICENSE("GPL");
