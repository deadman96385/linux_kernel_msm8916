// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * extcon-sm5502.c - Silicon Mitus SM5502 extcon drvier to support USB switches
 *
 * Copyright (c) 2014 Samsung Electronics Co., Ltd
 * Author: Chanwoo Choi <cw00.choi@samsung.com>
 */

#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/irqdomain.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/extcon-provider.h>

#include "extcon-sm5502.h"

#define DELAY_MS_DEFAULT		17000
#define DELAY_MS_SM5703		2700
#define DETECT_RETRY_MS			1000
#define DETECT_RETRY_MAX		10
#define SM5703_CONTROL_RESET_DEFAULT	0x1f

struct muic_irq {
	unsigned int irq;
	const char *name;
};

struct sm5502_muic_info;

struct sm5502_muic_irq_context {
	struct sm5502_muic_info *info;
	unsigned int type;
	unsigned int virq;
};

struct reg_data {
	u8 reg;
	unsigned int val;
	bool invert;
};

struct sm5502_muic_info {
	struct device *dev;
	struct extcon_dev *edev;

	struct i2c_client *i2c;
	struct regmap *regmap;

	const struct sm5502_type *type;
	struct regmap_irq_chip_data *irq_data;
	struct sm5502_muic_irq_context *irq_contexts;
	int irq;
	bool irq_pending;
	bool irq_work_scheduled;
	/* Protects IRQ rescan and work scheduling state. */
	spinlock_t irq_lock;
	struct work_struct irq_work;
	unsigned int prev_cable_type;
	unsigned int detect_retries;

	/* Serializes cable detection, switch programming and extcon updates. */
	struct mutex mutex;

	/*
	 * Use delayed workqueue to detect cable state and then
	 * notify cable state to notifiee/platform through uevent.
	 * After completing the booting of platform, the extcon provider
	 * driver should notify cable state to upper layer.
	 */
	struct delayed_work wq_detcable;
};

struct sm5502_type {
	const struct muic_irq *muic_irqs;
	unsigned int num_muic_irqs;
	const struct regmap_irq_chip *irq_chip;

	struct reg_data *reg_data;
	unsigned int num_reg_data;

	unsigned int otg_dev_type1;
	unsigned int usb_dev_type3;
	unsigned int dcp_dev_type3;
	unsigned int vbus_valid_reg;
	unsigned int vbus_valid_mask;
	unsigned int detect_delay_ms;
	unsigned int usb_vbus_sw;
	bool force_manual_path;
	bool ack_irqs_before_enable;
	int (*parse_irq)(struct sm5502_muic_info *info, int irq_type);
};

/* Default value of SM5502 register to bring up MUIC device. */
static struct reg_data sm5502_reg_data[] = {
	{
		.reg = SM5502_REG_RESET,
		.val = SM5502_REG_RESET_MASK,
		.invert = true,
	}, {
		.reg = SM5502_REG_CONTROL,
		.val = SM5502_REG_CONTROL_MASK_INT_MASK,
		.invert = false,
	}, {
		.reg = SM5502_REG_INTMASK1,
		.val = SM5502_REG_INTM1_KP_MASK
			| SM5502_REG_INTM1_LKP_MASK
			| SM5502_REG_INTM1_LKR_MASK,
		.invert = true,
	}, {
		.reg = SM5502_REG_INTMASK2,
		.val = SM5502_REG_INTM2_VBUS_DET_MASK
			| SM5502_REG_INTM2_REV_ACCE_MASK
			| SM5502_REG_INTM2_ADC_CHG_MASK
			| SM5502_REG_INTM2_STUCK_KEY_MASK
			| SM5502_REG_INTM2_STUCK_KEY_RCV_MASK
			| SM5502_REG_INTM2_MHL_MASK,
		.invert = true,
	},
};

/* Default value of SM5504 register to bring up MUIC device. */
static struct reg_data sm5504_reg_data[] = {
	{
		.reg = SM5502_REG_RESET,
		.val = SM5502_REG_RESET_MASK,
		.invert = true,
	}, {
		.reg = SM5502_REG_INTMASK1,
		.val = SM5504_REG_INTM1_ATTACH_MASK
			| SM5504_REG_INTM1_DETACH_MASK,
		.invert = false,
	}, {
		.reg = SM5502_REG_INTMASK2,
		.val = SM5504_REG_INTM2_RID_CHG_MASK
			| SM5504_REG_INTM2_UVLO_MASK
			| SM5504_REG_INTM2_POR_MASK,
		.invert = true,
	}, {
		.reg = SM5502_REG_CONTROL,
		.val = SM5502_REG_CONTROL_MANUAL_SW_MASK
			| SM5504_REG_CONTROL_CHGTYP_MASK
			| SM5504_REG_CONTROL_USBCHDEN_MASK
			| SM5504_REG_CONTROL_ADC_EN_MASK,
		.invert = true,
	},
};

/* Register setup used by Samsung's downstream SM5703 MUIC driver. */
static struct reg_data sm5703_reg_data[] = {
	{
		.reg = SM5502_REG_CONTROL,
		.val = SM5502_REG_CONTROL_SW_OPEN_MASK
			| SM5502_REG_CONTROL_RAW_DATA_MASK
			| SM5502_REG_CONTROL_MANUAL_SW_MASK
			| SM5502_REG_CONTROL_WAIT_MASK,
		.invert = true,
	}, {
		.reg = SM5502_REG_TIMING_SET1,
		.val = TIMING_ADC_DET_300MS,
		.invert = true,
	}, {
		.reg = SM5703_REG_CHGPUMP_SET,
		.val = 0,
		.invert = true,
	},
};

/* List of detectable cables */
static const unsigned int sm5502_extcon_cable[] = {
	EXTCON_USB,
	EXTCON_USB_HOST,
	EXTCON_CHG_USB_SDP,
	EXTCON_CHG_USB_CDP,
	EXTCON_CHG_USB_DCP,
	EXTCON_NONE,
};

/* Define supported accessory type */
enum sm5502_muic_acc_type {
	SM5502_MUIC_ADC_GROUND = 0x0,
	SM5502_MUIC_ADC_SEND_END_BUTTON,
	SM5502_MUIC_ADC_REMOTE_S1_BUTTON,
	SM5502_MUIC_ADC_REMOTE_S2_BUTTON,
	SM5502_MUIC_ADC_REMOTE_S3_BUTTON,
	SM5502_MUIC_ADC_REMOTE_S4_BUTTON,
	SM5502_MUIC_ADC_REMOTE_S5_BUTTON,
	SM5502_MUIC_ADC_REMOTE_S6_BUTTON,
	SM5502_MUIC_ADC_REMOTE_S7_BUTTON,
	SM5502_MUIC_ADC_REMOTE_S8_BUTTON,
	SM5502_MUIC_ADC_REMOTE_S9_BUTTON,
	SM5502_MUIC_ADC_REMOTE_S10_BUTTON,
	SM5502_MUIC_ADC_REMOTE_S11_BUTTON,
	SM5502_MUIC_ADC_REMOTE_S12_BUTTON,
	SM5502_MUIC_ADC_RESERVED_ACC_1,
	SM5502_MUIC_ADC_RESERVED_ACC_2,
	SM5502_MUIC_ADC_RESERVED_ACC_3,
	SM5502_MUIC_ADC_RESERVED_ACC_4,
	SM5502_MUIC_ADC_RESERVED_ACC_5,
	SM5502_MUIC_ADC_AUDIO_TYPE2,
	SM5502_MUIC_ADC_PHONE_POWERED_DEV,
	SM5502_MUIC_ADC_TTY_CONVERTER,
	SM5502_MUIC_ADC_UART_CABLE,
	SM5502_MUIC_ADC_TYPE1_CHARGER,
	SM5502_MUIC_ADC_FACTORY_MODE_BOOT_OFF_USB,
	SM5502_MUIC_ADC_FACTORY_MODE_BOOT_ON_USB,
	SM5502_MUIC_ADC_AUDIO_VIDEO_CABLE,
	SM5502_MUIC_ADC_TYPE2_CHARGER,
	SM5502_MUIC_ADC_FACTORY_MODE_BOOT_OFF_UART,
	SM5502_MUIC_ADC_FACTORY_MODE_BOOT_ON_UART,
	SM5502_MUIC_ADC_AUDIO_TYPE1,
	SM5502_MUIC_ADC_OPEN = 0x1f,

	/*
	 * The below accessories have same ADC value (0x1f or 0x1e).
	 * So, Device type1 is used to separate specific accessory.
	 */
							/* |---------|--ADC| */
							/* |    [7:5]|[4:0]| */
	SM5502_MUIC_ADC_AUDIO_TYPE1_FULL_REMOTE = 0x3e,	/* |      001|11110| */
	SM5502_MUIC_ADC_AUDIO_TYPE1_SEND_END = 0x5e,	/* |      010|11110| */
							/* |Dev Type1|--ADC| */
	SM5502_MUIC_ADC_GROUND_USB_OTG = 0x80,		/* |      100|00000| */
	SM5502_MUIC_ADC_OPEN_USB_CDP = 0x3f,		/* |      001|11111| */
	SM5502_MUIC_ADC_OPEN_USB = 0x5f,		/* |      010|11111| */
	SM5502_MUIC_ADC_OPEN_TA = 0xdf,			/* |      110|11111| */
	SM5502_MUIC_ADC_OPEN_USB_OTG = 0xff,		/* |      111|11111| */
};

/* List of supported interrupt for SM5502 */
static const struct muic_irq sm5502_muic_irqs[] = {
	{ SM5502_IRQ_INT1_ATTACH,	"muic-attach" },
	{ SM5502_IRQ_INT1_DETACH,	"muic-detach" },
	{ SM5502_IRQ_INT1_KP,		"muic-kp" },
	{ SM5502_IRQ_INT1_LKP,		"muic-lkp" },
	{ SM5502_IRQ_INT1_LKR,		"muic-lkr" },
	{ SM5502_IRQ_INT1_OVP_EVENT,	"muic-ovp-event" },
	{ SM5502_IRQ_INT1_OCP_EVENT,	"muic-ocp-event" },
	{ SM5502_IRQ_INT1_OVP_OCP_DIS,	"muic-ovp-ocp-dis" },
	{ SM5502_IRQ_INT2_VBUS_DET,	"muic-vbus-det" },
	{ SM5502_IRQ_INT2_REV_ACCE,	"muic-rev-acce" },
	{ SM5502_IRQ_INT2_ADC_CHG,	"muic-adc-chg" },
	{ SM5502_IRQ_INT2_STUCK_KEY,	"muic-stuck-key" },
	{ SM5502_IRQ_INT2_STUCK_KEY_RCV, "muic-stuck-key-rcv" },
	{ SM5502_IRQ_INT2_MHL,		"muic-mhl" },
};

/* Define interrupt list of SM5502 to register regmap_irq */
static const struct regmap_irq sm5502_irqs[] = {
	/* INT1 interrupts */
	{ .reg_offset = 0, .mask = SM5502_IRQ_INT1_ATTACH_MASK, },
	{ .reg_offset = 0, .mask = SM5502_IRQ_INT1_DETACH_MASK, },
	{ .reg_offset = 0, .mask = SM5502_IRQ_INT1_KP_MASK, },
	{ .reg_offset = 0, .mask = SM5502_IRQ_INT1_LKP_MASK, },
	{ .reg_offset = 0, .mask = SM5502_IRQ_INT1_LKR_MASK, },
	{ .reg_offset = 0, .mask = SM5502_IRQ_INT1_OVP_EVENT_MASK, },
	{ .reg_offset = 0, .mask = SM5502_IRQ_INT1_OCP_EVENT_MASK, },
	{ .reg_offset = 0, .mask = SM5502_IRQ_INT1_OVP_OCP_DIS_MASK, },

	/* INT2 interrupts */
	{ .reg_offset = 1, .mask = SM5502_IRQ_INT2_VBUS_DET_MASK,},
	{ .reg_offset = 1, .mask = SM5502_IRQ_INT2_REV_ACCE_MASK, },
	{ .reg_offset = 1, .mask = SM5502_IRQ_INT2_ADC_CHG_MASK, },
	{ .reg_offset = 1, .mask = SM5502_IRQ_INT2_STUCK_KEY_MASK, },
	{ .reg_offset = 1, .mask = SM5502_IRQ_INT2_STUCK_KEY_RCV_MASK, },
	{ .reg_offset = 1, .mask = SM5502_IRQ_INT2_MHL_MASK, },
};

static const struct regmap_irq_chip sm5502_muic_irq_chip = {
	.name			= "sm5502",
	.status_base		= SM5502_REG_INT1,
	.mask_base		= SM5502_REG_INTMASK1,
	.num_regs		= 2,
	.irqs			= sm5502_irqs,
	.num_irqs		= ARRAY_SIZE(sm5502_irqs),
};

/*
 * SM5703 shares the register layout, but not the meaning of INT2.  Request
 * every interrupt that Samsung's downstream driver leaves unmasked.  The
 * remaining INT1 bits stay masked by regmap-irq.
 */
static const struct muic_irq sm5703_muic_irqs[] = {
	{ SM5703_IRQ_INT1_ATTACH,		"muic-attach" },
	{ SM5703_IRQ_INT1_DETACH,		"muic-detach" },
	{ SM5703_IRQ_INT1_OVP_ENABLE,		"muic-ovp-enable" },
	{ SM5703_IRQ_INT1_OVP_DISABLE,		"muic-ovp-disable" },
	{ SM5703_IRQ_INT2_VBUS_OFF,		"muic-vbus-off" },
	{ SM5703_IRQ_INT2_RESERVED_ATTACH,	"muic-reserved-attach" },
	{ SM5703_IRQ_INT2_ADC_CHANGE,		"muic-adc-change" },
	{ SM5703_IRQ_INT2_STUCK_KEY,		"muic-stuck-key" },
	{ SM5703_IRQ_INT2_STUCK_KEY_RCV,	"muic-stuck-key-rcv" },
	{ SM5703_IRQ_INT2_MHL,			"muic-mhl" },
	{ SM5703_IRQ_INT2_RID_CHARGER,		"muic-rid-charger" },
	{ SM5703_IRQ_INT2_VBUSDET_ON,		"muic-vbus-on" },
};

static const struct regmap_irq sm5703_irqs[] = {
	/* INT1 interrupts */
	{ .reg_offset = 0, .mask = SM5703_IRQ_INT1_ATTACH_MASK, },
	{ .reg_offset = 0, .mask = SM5703_IRQ_INT1_DETACH_MASK, },
	{ .reg_offset = 0, .mask = SM5703_IRQ_INT1_KP_MASK, },
	{ .reg_offset = 0, .mask = SM5703_IRQ_INT1_LKP_MASK, },
	{ .reg_offset = 0, .mask = SM5703_IRQ_INT1_LKR_MASK, },
	{ .reg_offset = 0, .mask = SM5703_IRQ_INT1_OVP_ENABLE_MASK, },
	{ .reg_offset = 0, .mask = SM5703_IRQ_INT1_RESERVED_MASK, },
	{ .reg_offset = 0, .mask = SM5703_IRQ_INT1_OVP_DISABLE_MASK, },

	/* INT2 interrupts */
	{ .reg_offset = 1, .mask = SM5703_IRQ_INT2_VBUS_OFF_MASK, },
	{ .reg_offset = 1, .mask = SM5703_IRQ_INT2_RESERVED_ATTACH_MASK, },
	{ .reg_offset = 1, .mask = SM5703_IRQ_INT2_ADC_CHANGE_MASK, },
	{ .reg_offset = 1, .mask = SM5703_IRQ_INT2_STUCK_KEY_MASK, },
	{ .reg_offset = 1, .mask = SM5703_IRQ_INT2_STUCK_KEY_RCV_MASK, },
	{ .reg_offset = 1, .mask = SM5703_IRQ_INT2_MHL_MASK, },
	{ .reg_offset = 1, .mask = SM5703_IRQ_INT2_RID_CHARGER_MASK, },
	{ .reg_offset = 1, .mask = SM5703_IRQ_INT2_VBUSDET_ON_MASK, },
};

static const struct regmap_irq_chip sm5703_muic_irq_chip = {
	.name			= "sm5703-muic",
	.status_base		= SM5502_REG_INT1,
	.mask_base		= SM5502_REG_INTMASK1,
	.num_regs		= 2,
	.irqs			= sm5703_irqs,
	.num_irqs		= ARRAY_SIZE(sm5703_irqs),
};

/* List of supported interrupt for SM5504 */
static const struct muic_irq sm5504_muic_irqs[] = {
	{ SM5504_IRQ_INT1_ATTACH,	"muic-attach" },
	{ SM5504_IRQ_INT1_DETACH,	"muic-detach" },
	{ SM5504_IRQ_INT1_CHG_DET,	"muic-chg-det" },
	{ SM5504_IRQ_INT1_DCD_OUT,	"muic-dcd-out" },
	{ SM5504_IRQ_INT1_OVP_EVENT,	"muic-ovp-event" },
	{ SM5504_IRQ_INT1_CONNECT,	"muic-connect" },
	{ SM5504_IRQ_INT1_ADC_CHG,	"muic-adc-chg" },
	{ SM5504_IRQ_INT2_RID_CHG,	"muic-rid-chg" },
	{ SM5504_IRQ_INT2_UVLO,		"muic-uvlo" },
	{ SM5504_IRQ_INT2_POR,		"muic-por" },
	{ SM5504_IRQ_INT2_OVP_FET,	"muic-ovp-fet" },
	{ SM5504_IRQ_INT2_OCP_LATCH,	"muic-ocp-latch" },
	{ SM5504_IRQ_INT2_OCP_EVENT,	"muic-ocp-event" },
	{ SM5504_IRQ_INT2_OVP_OCP_EVENT, "muic-ovp-ocp-event" },
};

/* Define interrupt list of SM5504 to register regmap_irq */
static const struct regmap_irq sm5504_irqs[] = {
	/* INT1 interrupts */
	{ .reg_offset = 0, .mask = SM5504_IRQ_INT1_ATTACH_MASK, },
	{ .reg_offset = 0, .mask = SM5504_IRQ_INT1_DETACH_MASK, },
	{ .reg_offset = 0, .mask = SM5504_IRQ_INT1_CHG_DET_MASK, },
	{ .reg_offset = 0, .mask = SM5504_IRQ_INT1_DCD_OUT_MASK, },
	{ .reg_offset = 0, .mask = SM5504_IRQ_INT1_OVP_MASK, },
	{ .reg_offset = 0, .mask = SM5504_IRQ_INT1_CONNECT_MASK, },
	{ .reg_offset = 0, .mask = SM5504_IRQ_INT1_ADC_CHG_MASK, },

	/* INT2 interrupts */
	{ .reg_offset = 1, .mask = SM5504_IRQ_INT2_RID_CHG_MASK,},
	{ .reg_offset = 1, .mask = SM5504_IRQ_INT2_UVLO_MASK, },
	{ .reg_offset = 1, .mask = SM5504_IRQ_INT2_POR_MASK, },
	{ .reg_offset = 1, .mask = SM5504_IRQ_INT2_OVP_FET_MASK, },
	{ .reg_offset = 1, .mask = SM5504_IRQ_INT2_OCP_LATCH_MASK, },
	{ .reg_offset = 1, .mask = SM5504_IRQ_INT2_OCP_EVENT_MASK, },
	{ .reg_offset = 1, .mask = SM5504_IRQ_INT2_OVP_OCP_EVENT_MASK, },
};

static const struct regmap_irq_chip sm5504_muic_irq_chip = {
	.name			= "sm5504",
	.status_base		= SM5502_REG_INT1,
	.mask_base		= SM5502_REG_INTMASK1,
	.num_regs		= 2,
	.irqs			= sm5504_irqs,
	.num_irqs		= ARRAY_SIZE(sm5504_irqs),
};

/* Define regmap configuration of SM5502 for I2C communication  */
static bool sm5502_muic_volatile_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case SM5502_REG_INTMASK1:
	case SM5502_REG_INTMASK2:
		return true;
	default:
		break;
	}
	return false;
}

static const struct regmap_config sm5502_muic_regmap_config = {
	.reg_bits	= 8,
	.val_bits	= 8,
	.volatile_reg	= sm5502_muic_volatile_reg,
	.max_register	= SM5502_REG_END,
};

/* Change DM_CON/DP_CON/VBUSIN switch according to cable type */
static int sm5502_muic_set_path(struct sm5502_muic_info *info,
				unsigned int con_sw, unsigned int vbus_sw,
				bool attached)
{
	int ret;

	if (!attached) {
		con_sw	= DM_DP_SWITCH_OPEN;
		vbus_sw	= VBUSIN_SWITCH_OPEN;
	}

	switch (con_sw) {
	case DM_DP_SWITCH_OPEN:
	case DM_DP_SWITCH_USB:
	case DM_DP_SWITCH_AUDIO:
	case DM_DP_SWITCH_UART:
		ret = regmap_update_bits(info->regmap, SM5502_REG_MANUAL_SW1,
					 SM5502_REG_MANUAL_SW1_DP_MASK |
					 SM5502_REG_MANUAL_SW1_DM_MASK,
					 con_sw);
		if (ret < 0) {
			dev_err(info->dev,
				"cannot update DM_CON/DP_CON switch\n");
			return ret;
		}
		break;
	default:
		dev_err(info->dev,
			"Unknown DM_CON/DP_CON switch type (%d)\n", con_sw);
		return -EINVAL;
	}

	switch (vbus_sw) {
	case VBUSIN_SWITCH_OPEN:
	case VBUSIN_SWITCH_VBUSOUT:
	case VBUSIN_SWITCH_MIC:
	case VBUSIN_SWITCH_VBUSOUT_WITH_USB:
		ret = regmap_update_bits(info->regmap, SM5502_REG_MANUAL_SW1,
					 SM5502_REG_MANUAL_SW1_VBUSIN_MASK,
					 vbus_sw);
		if (ret < 0) {
			dev_err(info->dev,
				"cannot update VBUSIN switch\n");
			return ret;
		}
		break;
	default:
		dev_err(info->dev, "Unknown VBUS switch type (%d)\n", vbus_sw);
		return -EINVAL;
	}

	/*
	 * SM5703 powers up with automatic switching enabled.  MANUAL_SW1 writes
	 * are ignored in that mode, so explicitly select the path just programmed.
	 * Return to automatic detection after detach, matching the downstream
	 * driver's detach sequence.
	 */
	if (info->type->force_manual_path) {
		ret = regmap_update_bits(info->regmap, SM5502_REG_CONTROL,
					 SM5502_REG_CONTROL_MANUAL_SW_MASK,
					 attached ? 0 :
					 SM5502_REG_CONTROL_MANUAL_SW_MASK);
		if (ret) {
			dev_err(info->dev, "cannot select MUIC switch mode\n");
			return ret;
		}
	}
	dev_dbg(info->dev, "switch path: dpdm=0x%x vbus=0x%x attached=%u\n",
		con_sw, vbus_sw, attached);

	return 0;
}

static int sm5502_muic_unresolved_cable(struct sm5502_muic_info *info,
					unsigned int adc,
					unsigned int dev_type1)
{
	unsigned int vbus;
	int ret;

	if (!info->type->vbus_valid_mask)
		return -EINVAL;

	ret = regmap_read(info->regmap, info->type->vbus_valid_reg, &vbus);
	if (ret)
		return ret;

	if (!(vbus & info->type->vbus_valid_mask))
		return -ENODEV;

	dev_dbg(info->dev,
		"cable classification incomplete: adc=0x%x dev_type1=0x%x vbus=0x%x\n",
		adc, dev_type1, vbus);

	return -EAGAIN;
}

/* Return cable type of attached or detached accessories */
static int sm5502_muic_get_cable_type(struct sm5502_muic_info *info)
{
	unsigned int cable_type, adc, dev_type1, dev_type3 = 0, vbus;
	int ret;

	/* Read ADC value according to external cable or button */
	ret = regmap_read(info->regmap, SM5502_REG_ADC, &adc);
	if (ret) {
		dev_err(info->dev, "failed to read ADC register\n");
		return ret;
	}

	/*
	 * If ADC is SM5502_MUIC_ADC_GROUND(0x0), external cable hasn't
	 * connected with to MUIC device.
	 */
	cable_type = adc & SM5502_REG_ADC_MASK;

	switch (cable_type) {
	case SM5502_MUIC_ADC_GROUND:
		ret = regmap_read(info->regmap, SM5502_REG_DEV_TYPE1,
				  &dev_type1);
		if (ret) {
			dev_err(info->dev, "failed to read DEV_TYPE1 reg\n");
			return ret;
		}

		if (dev_type1 & info->type->otg_dev_type1) {
			cable_type = SM5502_MUIC_ADC_GROUND_USB_OTG;
		} else {
			dev_dbg(info->dev,
				"cannot identify the cable type: adc(0x%x), dev_type1(0x%x)\n",
				adc, dev_type1);
			return sm5502_muic_unresolved_cable(info, adc,
							       dev_type1);
		}
		break;
	case SM5502_MUIC_ADC_SEND_END_BUTTON:
	case SM5502_MUIC_ADC_REMOTE_S1_BUTTON:
	case SM5502_MUIC_ADC_REMOTE_S2_BUTTON:
	case SM5502_MUIC_ADC_REMOTE_S3_BUTTON:
	case SM5502_MUIC_ADC_REMOTE_S4_BUTTON:
	case SM5502_MUIC_ADC_REMOTE_S5_BUTTON:
	case SM5502_MUIC_ADC_REMOTE_S6_BUTTON:
	case SM5502_MUIC_ADC_REMOTE_S7_BUTTON:
	case SM5502_MUIC_ADC_REMOTE_S8_BUTTON:
	case SM5502_MUIC_ADC_REMOTE_S9_BUTTON:
	case SM5502_MUIC_ADC_REMOTE_S10_BUTTON:
	case SM5502_MUIC_ADC_REMOTE_S11_BUTTON:
	case SM5502_MUIC_ADC_REMOTE_S12_BUTTON:
	case SM5502_MUIC_ADC_RESERVED_ACC_1:
	case SM5502_MUIC_ADC_RESERVED_ACC_2:
	case SM5502_MUIC_ADC_RESERVED_ACC_3:
	case SM5502_MUIC_ADC_RESERVED_ACC_4:
	case SM5502_MUIC_ADC_RESERVED_ACC_5:
	case SM5502_MUIC_ADC_AUDIO_TYPE2:
	case SM5502_MUIC_ADC_PHONE_POWERED_DEV:
	case SM5502_MUIC_ADC_TTY_CONVERTER:
	case SM5502_MUIC_ADC_UART_CABLE:
	case SM5502_MUIC_ADC_TYPE1_CHARGER:
	case SM5502_MUIC_ADC_FACTORY_MODE_BOOT_OFF_USB:
	case SM5502_MUIC_ADC_FACTORY_MODE_BOOT_ON_USB:
	case SM5502_MUIC_ADC_AUDIO_VIDEO_CABLE:
	case SM5502_MUIC_ADC_TYPE2_CHARGER:
	case SM5502_MUIC_ADC_FACTORY_MODE_BOOT_OFF_UART:
	case SM5502_MUIC_ADC_FACTORY_MODE_BOOT_ON_UART:
		break;
	case SM5502_MUIC_ADC_AUDIO_TYPE1:
		/*
		 * Check whether cable type is
		 * SM5502_MUIC_ADC_AUDIO_TYPE1_FULL_REMOTE
		 * or SM5502_MUIC_ADC_AUDIO_TYPE1_SEND_END
		 * by using Button event.
		 */
		break;
	case SM5502_MUIC_ADC_OPEN:
		ret = regmap_read(info->regmap, SM5502_REG_DEV_TYPE1,
				  &dev_type1);
		if (ret) {
			dev_err(info->dev, "failed to read DEV_TYPE1 reg\n");
			return ret;
		}

		if (info->type->usb_dev_type3 || info->type->dcp_dev_type3) {
			ret = regmap_read(info->regmap, SM5502_REG_DEV_TYPE3,
					  &dev_type3);
			if (ret) {
				dev_err(info->dev, "failed to read DEV_TYPE3 reg\n");
				return ret;
			}
		}

		if (dev_type1 & info->type->otg_dev_type1) {
			cable_type = SM5502_MUIC_ADC_OPEN_USB_OTG;
			break;
		}

		if ((dev_type1 & SM5502_REG_DEV_TYPE1_USB_SDP_MASK) ||
		    (dev_type3 & info->type->usb_dev_type3)) {
			cable_type = SM5502_MUIC_ADC_OPEN_USB;
		} else if (dev_type1 & SM5502_REG_DEV_TYPE1_USB_CHG_MASK) {
			cable_type = SM5502_MUIC_ADC_OPEN_USB_CDP;
		} else if ((dev_type1 &
			    (SM5502_REG_DEV_TYPE1_DEDICATED_CHG_MASK |
			     SM5502_REG_DEV_TYPE1_CAR_KIT_CHARGER_MASK)) ||
			   (dev_type3 & info->type->dcp_dev_type3)) {
			cable_type = SM5502_MUIC_ADC_OPEN_TA;
		} else {
			dev_dbg(info->dev,
				"cannot identify the cable type: adc(0x%x), dev_type1(0x%x)\n",
				adc, dev_type1);
			return sm5502_muic_unresolved_cable(info, adc,
							       dev_type1);
		}
		break;
	default:
		dev_err(info->dev,
			"failed to identify the cable type: adc(0x%x)\n", adc);
		return -EINVAL;
	}

	if (info->type->vbus_valid_mask &&
	    cable_type != SM5502_MUIC_ADC_GROUND_USB_OTG &&
	    cable_type != SM5502_MUIC_ADC_OPEN_USB_OTG) {
		ret = regmap_read(info->regmap, info->type->vbus_valid_reg,
				  &vbus);
		if (ret) {
			dev_err(info->dev, "failed to read VBUS status reg\n");
			return ret;
		}
		if (!(vbus & info->type->vbus_valid_mask))
			return -ENODEV;
	}

	return cable_type;
}

static bool sm5502_muic_cable_supported(int cable_type)
{
	switch (cable_type) {
	case SM5502_MUIC_ADC_OPEN_USB:
	case SM5502_MUIC_ADC_OPEN_USB_CDP:
	case SM5502_MUIC_ADC_OPEN_TA:
	case SM5502_MUIC_ADC_GROUND_USB_OTG:
	case SM5502_MUIC_ADC_OPEN_USB_OTG:
		return true;
	default:
		return false;
	}
}

static int sm5502_muic_cable_handler(struct sm5502_muic_info *info,
				     int cable_type, bool attached)
{
	unsigned int con_sw = DM_DP_SWITCH_OPEN;
	unsigned int vbus_sw = VBUSIN_SWITCH_OPEN;
	unsigned int id;
	int ret, charger_id = EXTCON_NONE;

	switch (cable_type) {
	case SM5502_MUIC_ADC_OPEN_USB:
		id	= EXTCON_USB;
		charger_id = EXTCON_CHG_USB_SDP;
		con_sw	= DM_DP_SWITCH_USB;
		vbus_sw	= info->type->usb_vbus_sw;
		break;
	case SM5502_MUIC_ADC_OPEN_USB_CDP:
		id	= EXTCON_USB;
		charger_id = EXTCON_CHG_USB_CDP;
		con_sw	= DM_DP_SWITCH_USB;
		vbus_sw	= info->type->usb_vbus_sw;
		break;
	case SM5502_MUIC_ADC_OPEN_TA:
		id	= EXTCON_CHG_USB_DCP;
		con_sw	= DM_DP_SWITCH_OPEN;
		vbus_sw	= VBUSIN_SWITCH_VBUSOUT;
		break;
	case SM5502_MUIC_ADC_GROUND_USB_OTG:
	case SM5502_MUIC_ADC_OPEN_USB_OTG:
		id	= EXTCON_USB_HOST;
		con_sw	= DM_DP_SWITCH_USB;
		vbus_sw	= VBUSIN_SWITCH_OPEN;
		break;
	default:
		dev_dbg(info->dev,
			"cannot handle this cable_type (0x%x)\n", cable_type);
		return 0;
	}

	/* Change internal hardware path(DM_CON/DP_CON, VBUSIN) */
	ret = sm5502_muic_set_path(info, con_sw, vbus_sw, attached);
	if (ret < 0)
		return ret;

	/* Change the state of external accessory */
	ret = extcon_set_state_sync(info->edev, id, attached);
	if (ret)
		return ret;

	if (charger_id != EXTCON_NONE) {
		ret = extcon_set_state_sync(info->edev, charger_id, attached);
		if (ret)
			return ret;
	}

	return 0;
}

/* Reconcile extcon state with the final state reported by the MUIC. */
static int sm5502_muic_update_cable(struct sm5502_muic_info *info,
				    bool force_path)
{
	int cable_type, ret;

	cable_type = sm5502_muic_get_cable_type(info);
	if (cable_type == -EINVAL || cable_type == -ENODEV)
		cable_type = SM5502_MUIC_ADC_GROUND;
	else if (cable_type < 0)
		return cable_type;

	if (!sm5502_muic_cable_supported(cable_type))
		cable_type = SM5502_MUIC_ADC_GROUND;
	dev_dbg(info->dev, "cable state: current=0x%x previous=0x%x restore=%u\n",
		cable_type, info->prev_cable_type, force_path);

	if (cable_type == info->prev_cable_type) {
		if (force_path && sm5502_muic_cable_supported(cable_type))
			return sm5502_muic_cable_handler(info, cable_type, true);
		return 0;
	}

	if (sm5502_muic_cable_supported(info->prev_cable_type)) {
		ret = sm5502_muic_cable_handler(info, info->prev_cable_type, false);
		if (ret)
			return ret;
		info->prev_cable_type = SM5502_MUIC_ADC_GROUND;
	}

	if (!sm5502_muic_cable_supported(cable_type))
		return 0;

	ret = sm5502_muic_cable_handler(info, cable_type, true);
	if (!ret)
		info->prev_cable_type = cable_type;

	return ret;
}

static void sm5502_muic_irq_work(struct work_struct *work)
{
	struct sm5502_muic_info *info = container_of(work,
			struct sm5502_muic_info, irq_work);
	unsigned long flags;
	bool pending;
	int ret;

	if (IS_ERR_OR_NULL(info->edev)) {
		spin_lock_irqsave(&info->irq_lock, flags);
		info->irq_pending = false;
		info->irq_work_scheduled = false;
		spin_unlock_irqrestore(&info->irq_lock, flags);
		return;
	}

	for (;;) {
		spin_lock_irqsave(&info->irq_lock, flags);
		pending = info->irq_pending;
		info->irq_pending = false;
		if (!pending)
			info->irq_work_scheduled = false;
		spin_unlock_irqrestore(&info->irq_lock, flags);

		if (!pending)
			break;

		mutex_lock(&info->mutex);
		ret = sm5502_muic_update_cable(info, false);
		mutex_unlock(&info->mutex);
		if (ret == -EAGAIN) {
			mod_delayed_work(system_power_efficient_wq,
					 &info->wq_detcable,
					 msecs_to_jiffies(DETECT_RETRY_MS));
		} else if (ret) {
			dev_err(info->dev,
				"failed to rescan MUIC cable state: %d\n", ret);
		}
	}
}

/* Mark cable-state interrupts for a hardware rescan. */
static int sm5502_parse_irq(struct sm5502_muic_info *info, int irq_type)
{
	switch (irq_type) {
	case SM5502_IRQ_INT1_ATTACH:
	case SM5502_IRQ_INT1_DETACH:
		info->irq_pending = true;
		break;
	case SM5502_IRQ_INT1_KP:
	case SM5502_IRQ_INT1_LKP:
	case SM5502_IRQ_INT1_LKR:
	case SM5502_IRQ_INT1_OVP_EVENT:
	case SM5502_IRQ_INT1_OCP_EVENT:
	case SM5502_IRQ_INT1_OVP_OCP_DIS:
	case SM5502_IRQ_INT2_VBUS_DET:
	case SM5502_IRQ_INT2_REV_ACCE:
	case SM5502_IRQ_INT2_ADC_CHG:
	case SM5502_IRQ_INT2_STUCK_KEY:
	case SM5502_IRQ_INT2_STUCK_KEY_RCV:
	case SM5502_IRQ_INT2_MHL:
	default:
		break;
	}

	return 0;
}

static int sm5504_parse_irq(struct sm5502_muic_info *info, int irq_type)
{
	switch (irq_type) {
	case SM5504_IRQ_INT1_ATTACH:
	case SM5504_IRQ_INT1_DETACH:
		info->irq_pending = true;
		break;
	case SM5504_IRQ_INT1_CHG_DET:
	case SM5504_IRQ_INT1_DCD_OUT:
	case SM5504_IRQ_INT1_OVP_EVENT:
	case SM5504_IRQ_INT1_CONNECT:
	case SM5504_IRQ_INT1_ADC_CHG:
	case SM5504_IRQ_INT2_RID_CHG:
	case SM5504_IRQ_INT2_UVLO:
	case SM5504_IRQ_INT2_POR:
	case SM5504_IRQ_INT2_OVP_FET:
	case SM5504_IRQ_INT2_OCP_LATCH:
	case SM5504_IRQ_INT2_OCP_EVENT:
	case SM5504_IRQ_INT2_OVP_OCP_EVENT:
	default:
		break;
	}

	return 0;
}

static int sm5703_parse_irq(struct sm5502_muic_info *info, int irq_type)
{
	switch (irq_type) {
	case SM5703_IRQ_INT1_ATTACH:
	case SM5703_IRQ_INT1_DETACH:
	case SM5703_IRQ_INT2_VBUS_OFF:
	case SM5703_IRQ_INT2_RESERVED_ATTACH:
	case SM5703_IRQ_INT2_ADC_CHANGE:
	case SM5703_IRQ_INT2_MHL:
	case SM5703_IRQ_INT2_RID_CHARGER:
	case SM5703_IRQ_INT2_VBUSDET_ON:
		info->irq_pending = true;
		break;
	case SM5703_IRQ_INT1_OVP_ENABLE:
	case SM5703_IRQ_INT1_OVP_DISABLE:
	case SM5703_IRQ_INT2_STUCK_KEY:
	case SM5703_IRQ_INT2_STUCK_KEY_RCV:
	default:
		break;
	}

	return 0;
}

static irqreturn_t sm5502_muic_irq_handler(int irq, void *data)
{
	struct sm5502_muic_irq_context *context = data;
	struct sm5502_muic_info *info = context->info;
	unsigned long flags;
	bool schedule = false;
	int ret;

	spin_lock_irqsave(&info->irq_lock, flags);
	ret = info->type->parse_irq(info, context->type);
	if (!ret && info->irq_pending &&
	    !info->irq_work_scheduled) {
		info->irq_work_scheduled = true;
		schedule = true;
	}
	spin_unlock_irqrestore(&info->irq_lock, flags);
	if (ret < 0) {
		dev_warn(info->dev, "cannot handle interrupt: %d\n",
			 context->type);
		return IRQ_HANDLED;
	}
	if (schedule)
		schedule_work(&info->irq_work);

	return IRQ_HANDLED;
}

static void sm5502_muic_detect_cable_wq(struct work_struct *work)
{
	struct sm5502_muic_info *info = container_of(to_delayed_work(work),
				struct sm5502_muic_info, wq_detcable);
	int ret;

	mutex_lock(&info->mutex);

	/* Notify the state of connector cable or not  */
	ret = sm5502_muic_update_cable(info, false);
	if (ret == -EAGAIN && info->detect_retries < DETECT_RETRY_MAX) {
		info->detect_retries++;
		mod_delayed_work(system_power_efficient_wq, &info->wq_detcable,
				 msecs_to_jiffies(DETECT_RETRY_MS));
	} else if (ret < 0) {
		dev_warn(info->dev, "failed to detect cable state\n");
	} else {
		info->detect_retries = 0;
	}

	mutex_unlock(&info->mutex);
}

static int sm5502_init_dev_type(struct sm5502_muic_info *info)
{
	unsigned int reg_data, vendor_id, version_id;
	int i, ret;

	/* To test I2C, Print version_id and vendor_id of SM5502 */
	ret = regmap_read(info->regmap, SM5502_REG_DEVICE_ID, &reg_data);
	if (ret) {
		dev_err(info->dev,
			"failed to read DEVICE_ID register: %d\n", ret);
		return ret;
	}

	vendor_id = ((reg_data & SM5502_REG_DEVICE_ID_VENDOR_MASK) >>
				SM5502_REG_DEVICE_ID_VENDOR_SHIFT);
	version_id = ((reg_data & SM5502_REG_DEVICE_ID_VERSION_MASK) >>
				SM5502_REG_DEVICE_ID_VERSION_SHIFT);

	dev_info(info->dev, "Device type: version: 0x%x, vendor: 0x%x\n",
		 version_id, vendor_id);

	/* Initiazle the register of SM5502 device to bring-up */
	for (i = 0; i < info->type->num_reg_data; i++) {
		unsigned int val = 0;

		if (!info->type->reg_data[i].invert)
			val |= ~info->type->reg_data[i].val;
		else
			val = info->type->reg_data[i].val;
		ret = regmap_write(info->regmap, info->type->reg_data[i].reg,
				   val);
		if (ret) {
			dev_err(info->dev,
				"failed to initialize register 0x%x: %d\n",
				info->type->reg_data[i].reg, ret);
			return ret;
		}
	}

	return 0;
}

static int sm5502_ack_initial_irqs(struct sm5502_muic_info *info)
{
	unsigned int val;
	int i, ret;

	if (!info->type->ack_irqs_before_enable)
		return 0;

	/* Samsung reads both clear-on-read status registers twice at startup. */
	for (i = 0; i < 2; i++) {
		ret = regmap_read(info->regmap, SM5502_REG_INT1, &val);
		if (ret)
			return ret;
		ret = regmap_read(info->regmap, SM5502_REG_INT2, &val);
		if (ret)
			return ret;
	}

	return 0;
}

static int sm5022_muic_i2c_probe(struct i2c_client *i2c)
{
	struct device_node *np = i2c->dev.of_node;
	struct sm5502_muic_info *info;
	int i, ret, irq_flags;

	if (!np)
		return -EINVAL;

	info = devm_kzalloc(&i2c->dev, sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;
	i2c_set_clientdata(i2c, info);

	info->dev = &i2c->dev;
	info->i2c = i2c;
	info->irq = i2c->irq;
	info->prev_cable_type = SM5502_MUIC_ADC_GROUND;
	info->type = device_get_match_data(info->dev);
	if (!info->type)
		return -EINVAL;
	if (!info->type->parse_irq) {
		dev_err(info->dev, "parse_irq missing in struct sm5502_type\n");
		return -EINVAL;
	}

	mutex_init(&info->mutex);
	spin_lock_init(&info->irq_lock);

	INIT_WORK(&info->irq_work, sm5502_muic_irq_work);
	INIT_DELAYED_WORK(&info->wq_detcable, sm5502_muic_detect_cable_wq);

	info->regmap = devm_regmap_init_i2c(i2c, &sm5502_muic_regmap_config);
	if (IS_ERR(info->regmap)) {
		ret = PTR_ERR(info->regmap);
		dev_err(info->dev, "failed to allocate register map: %d\n", ret);
		return ret;
	}

	if (info->type->ack_irqs_before_enable) {
		/* Initialize and clear stale events before enabling SM5703 IRQs. */
		ret = sm5502_init_dev_type(info);
		if (ret)
			return ret;

		ret = sm5502_ack_initial_irqs(info);
		if (ret)
			return dev_err_probe(info->dev, ret,
					     "failed to acknowledge initial interrupts\n");
	}

	/* Support irq domain for SM5502 MUIC device */
	irq_flags = IRQF_TRIGGER_FALLING | IRQF_ONESHOT | IRQF_SHARED;
	ret = devm_regmap_add_irq_chip(info->dev, info->regmap, info->irq,
				       irq_flags, 0, info->type->irq_chip,
				       &info->irq_data);
	if (ret != 0) {
		dev_err(info->dev, "failed to request IRQ %d: %d\n",
			info->irq, ret);
		return ret;
	}

	info->irq_contexts = devm_kcalloc(info->dev,
					  info->type->num_muic_irqs,
					  sizeof(*info->irq_contexts),
					  GFP_KERNEL);
	if (!info->irq_contexts)
		return -ENOMEM;

	for (i = 0; i < info->type->num_muic_irqs; i++) {
		const struct muic_irq *muic_irq = &info->type->muic_irqs[i];
		struct sm5502_muic_irq_context *context =
			&info->irq_contexts[i];
		int virq;

		virq = regmap_irq_get_virq(info->irq_data, muic_irq->irq);
		if (virq <= 0)
			return -EINVAL;
		context->info = info;
		context->type = muic_irq->irq;
		context->virq = virq;

		ret = devm_request_threaded_irq(info->dev, virq, NULL,
						sm5502_muic_irq_handler,
						IRQF_NO_SUSPEND | IRQF_ONESHOT,
						muic_irq->name, context);
		if (ret) {
			dev_err(info->dev,
				"failed: irq request (IRQ: %d, error :%d)\n",
				muic_irq->irq, ret);
			return ret;
		}
	}

	/* Allocate extcon device */
	info->edev = devm_extcon_dev_allocate(info->dev, sm5502_extcon_cable);
	if (IS_ERR(info->edev)) {
		dev_err(info->dev, "failed to allocate memory for extcon\n");
		return -ENOMEM;
	}

	/* Register extcon device */
	ret = devm_extcon_dev_register(info->dev, info->edev);
	if (ret) {
		dev_err(info->dev, "failed to register extcon device\n");
		return ret;
	}

	if (!info->type->ack_irqs_before_enable) {
		ret = sm5502_init_dev_type(info);
		if (ret)
			return ret;
	}

	/*
	 * Detect accessory after completing the initialization of platform
	 *
	 * - Use delayed workqueue to detect cable state and then
	 * notify cable state to notifiee/platform through uevent.
	 * After completing the booting of platform, the extcon provider
	 * driver should notify cable state to upper layer.
	 */
	queue_delayed_work(system_power_efficient_wq, &info->wq_detcable,
			   msecs_to_jiffies(info->type->detect_delay_ms));

	return 0;
}

static void sm5502_muic_i2c_remove(struct i2c_client *i2c)
{
	struct sm5502_muic_info *info = i2c_get_clientdata(i2c);
	int i;

	for (i = 0; i < info->type->num_muic_irqs; i++)
		devm_free_irq(info->dev, info->irq_contexts[i].virq,
			      &info->irq_contexts[i]);

	cancel_delayed_work_sync(&info->wq_detcable);
	cancel_work_sync(&info->irq_work);
}

static const struct sm5502_type sm5502_data = {
	.muic_irqs = sm5502_muic_irqs,
	.num_muic_irqs = ARRAY_SIZE(sm5502_muic_irqs),
	.irq_chip = &sm5502_muic_irq_chip,
	.reg_data = sm5502_reg_data,
	.num_reg_data = ARRAY_SIZE(sm5502_reg_data),
	.otg_dev_type1 = SM5502_REG_DEV_TYPE1_USB_OTG_MASK,
	.detect_delay_ms = DELAY_MS_DEFAULT,
	.usb_vbus_sw = VBUSIN_SWITCH_VBUSOUT_WITH_USB,
	.parse_irq = sm5502_parse_irq,
};

static const struct sm5502_type sm5504_data = {
	.muic_irqs = sm5504_muic_irqs,
	.num_muic_irqs = ARRAY_SIZE(sm5504_muic_irqs),
	.irq_chip = &sm5504_muic_irq_chip,
	.reg_data = sm5504_reg_data,
	.num_reg_data = ARRAY_SIZE(sm5504_reg_data),
	.otg_dev_type1 = SM5504_REG_DEV_TYPE1_USB_OTG_MASK,
	.detect_delay_ms = DELAY_MS_DEFAULT,
	.usb_vbus_sw = VBUSIN_SWITCH_VBUSOUT_WITH_USB,
	.parse_irq = sm5504_parse_irq,
};

static const struct sm5502_type sm5703_data = {
	.muic_irqs = sm5703_muic_irqs,
	.num_muic_irqs = ARRAY_SIZE(sm5703_muic_irqs),
	.irq_chip = &sm5703_muic_irq_chip,
	.reg_data = sm5703_reg_data,
	.num_reg_data = ARRAY_SIZE(sm5703_reg_data),
	.otg_dev_type1 = SM5502_REG_DEV_TYPE1_USB_OTG_MASK,
	.usb_dev_type3 = SM5703_REG_DEV_TYPE3_NON_STANDARD_MASK,
	.dcp_dev_type3 = SM5703_REG_DEV_TYPE3_U200_CHG_MASK,
	.vbus_valid_reg = SM5703_REG_VBUSINVALID,
	.vbus_valid_mask = SM5703_REG_VBUSIN_VALID_MASK,
	.detect_delay_ms = DELAY_MS_SM5703,
	.usb_vbus_sw = VBUSIN_SWITCH_VBUSOUT,
	.force_manual_path = true,
	.ack_irqs_before_enable = true,
	.parse_irq = sm5703_parse_irq,
};

static const struct of_device_id sm5502_dt_match[] = {
	{ .compatible = "siliconmitus,sm5502-muic", .data = &sm5502_data },
	{ .compatible = "siliconmitus,sm5504-muic", .data = &sm5504_data },
	{ .compatible = "siliconmitus,sm5703-muic", .data = &sm5703_data },
	{ },
};
MODULE_DEVICE_TABLE(of, sm5502_dt_match);

#ifdef CONFIG_PM_SLEEP
static int sm5502_muic_suspend(struct device *dev)
{
	struct i2c_client *i2c = to_i2c_client(dev);
	struct sm5502_muic_info *info = i2c_get_clientdata(i2c);

	enable_irq_wake(info->irq);

	return 0;
}

static int sm5502_muic_resume(struct device *dev)
{
	struct i2c_client *i2c = to_i2c_client(dev);
	struct sm5502_muic_info *info = i2c_get_clientdata(i2c);
	unsigned int control;
	int ret;

	disable_irq_wake(info->irq);

	mutex_lock(&info->mutex);
	if (info->type->force_manual_path) {
		ret = regmap_read(info->regmap, SM5502_REG_CONTROL, &control);
		if (!ret && control == SM5703_CONTROL_RESET_DEFAULT)
			ret = sm5502_init_dev_type(info);
	} else {
		ret = 0;
	}

	if (!ret)
		ret = sm5502_muic_update_cable(info, true);
	mutex_unlock(&info->mutex);

	if (ret == -EAGAIN) {
		info->detect_retries = 0;
		mod_delayed_work(system_power_efficient_wq, &info->wq_detcable,
				 msecs_to_jiffies(DETECT_RETRY_MS));
	} else if (ret) {
		dev_warn(info->dev, "failed to restore MUIC state: %d\n", ret);
	}

	return 0;
}
#endif

static SIMPLE_DEV_PM_OPS(sm5502_muic_pm_ops,
			 sm5502_muic_suspend, sm5502_muic_resume);

static const struct i2c_device_id sm5502_i2c_id[] = {
	{ "sm5502", (kernel_ulong_t)&sm5502_data },
	{ "sm5504", (kernel_ulong_t)&sm5504_data },
	{ "sm5703-muic", (kernel_ulong_t)&sm5703_data },
	{ }
};
MODULE_DEVICE_TABLE(i2c, sm5502_i2c_id);

static struct i2c_driver sm5502_muic_i2c_driver = {
	.driver		= {
		.name	= "sm5502",
		.pm	= &sm5502_muic_pm_ops,
		.of_match_table = sm5502_dt_match,
	},
	.probe = sm5022_muic_i2c_probe,
	.remove = sm5502_muic_i2c_remove,
	.id_table = sm5502_i2c_id,
};

static int __init sm5502_muic_i2c_init(void)
{
	return i2c_add_driver(&sm5502_muic_i2c_driver);
}
subsys_initcall(sm5502_muic_i2c_init);

MODULE_DESCRIPTION("Silicon Mitus SM5502 Extcon driver");
MODULE_AUTHOR("Chanwoo Choi <cw00.choi@samsung.com>");
MODULE_LICENSE("GPL");
