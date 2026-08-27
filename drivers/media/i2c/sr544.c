// SPDX-License-Identifier: GPL-2.0-only
/*
 * V4L2 driver for the SiliconFile SR544 image sensor.
 *
 * This implements the native rear-camera mode used by the Samsung GT510.
 * The firmware and register sequence were recovered from Samsung's shipping
 * camera library for this exact module.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/nvmem-provider.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>

#include <media/v4l2-cci.h>
#include <media/v4l2-common.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-mediabus.h>
#include <media/v4l2-subdev.h>

#define SR544_REG_CHIP_ID		CCI_REG16(0x0f16)
#define SR544_CHIP_ID			0x4405
#define SR544_REG_MODE_SELECT		CCI_REG16(0x0118)
#define SR544_MODE_STANDBY		0x0000
#define SR544_MODE_STREAMING		0x0100

#define SR544_REG_FRAME_LENGTH		CCI_REG16(0x0006)
#define SR544_FRAME_LENGTH_MAX		0xffff
#define SR544_REG_EXPOSURE		CCI_REG16(0x0004)
#define SR544_EXPOSURE_MIN		4
#define SR544_EXPOSURE_MARGIN		4
#define SR544_EXPOSURE_DEFAULT		0x07e0
#define SR544_REG_ANALOGUE_GAIN		CCI_REG8(0x003a)
#define SR544_ANALOGUE_GAIN_MIN		0x00
#define SR544_ANALOGUE_GAIN_MAX		0xf0
#define SR544_REG_DIGITAL_GAIN_GR	CCI_REG16(0x0508)
#define SR544_REG_DIGITAL_GAIN_GB	CCI_REG16(0x050a)
#define SR544_REG_DIGITAL_GAIN_R	CCI_REG16(0x050c)
#define SR544_REG_DIGITAL_GAIN_B	CCI_REG16(0x050e)
#define SR544_DIGITAL_GAIN_MIN		0x0100
#define SR544_DIGITAL_GAIN_MAX		0x07ff

#define SR544_REG_OTP_COMMAND		CCI_REG8(0x0102)
#define SR544_REG_OTP_ADDRESS_H		CCI_REG8(0x010a)
#define SR544_REG_OTP_ADDRESS_L		CCI_REG8(0x010b)
#define SR544_REG_OTP_READ_DATA		CCI_REG8(0x0108)
#define SR544_OTP_COMMAND_READ		0x01
#define SR544_OTP_BANK_SELECT_ADDRESS	0x0680
#define SR544_OTP_BANK_0_START		0x0690
#define SR544_OTP_BANK_1_START		0x0ee0
#define SR544_OTP_BANK_2_START		0x1730
#define SR544_OTP_VERSION_ADDRESS	0x0020
#define SR544_OTP_VERSION_OFFSET	0x0050
#define SR544_OTP_SIZE			0x0850

#define SR544_XCLK_FREQ			26000000
#define SR544_XCLK_MIN			25900000
#define SR544_XCLK_MAX			26100000
#define SR544_LINK_FREQ			440000000
#define SR544_PIXEL_RATE		176000000

struct sr544_mode {
	u32 width;
	u32 height;
	u32 line_length;
	u32 frame_length;
	const struct cci_reg_sequence *reg_list;
	unsigned int num_regs;
};

struct sr544 {
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
	const struct sr544_mode *cur_mode;
	struct v4l2_ctrl *hblank;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *exposure;
	u8 otp_data[SR544_OTP_SIZE];
	u8 otp_bank;
	u8 otp_version;

	s64 link_freq;
	bool otp_cached;
	bool streaming;
};

static inline struct sr544 *to_sr544(struct v4l2_subdev *sd)
{
	return container_of(sd, struct sr544, sd);
}

static const struct cci_reg_sequence sr544_init_regs[] = {
#include "sr544_regs.h"
};

static const struct cci_reg_sequence sr544_2592x1944_regs[] = {
	{ CCI_REG16(0x0b02), 0x0014 },
	{ CCI_REG16(0x0b04), 0x07c8 },
	{ CCI_REG16(0x0b06), 0x5ed7 },
	{ CCI_REG16(0x0b14), 0x5313 },
	{ CCI_REG16(0x0b16), 0x4a0b },
	{ CCI_REG16(0x0b18), 0x0000 },
	{ CCI_REG16(0x0b1a), 0x1044 },
	{ CCI_REG16(0x0012), 0x00aa },
	{ CCI_REG16(0x0018), 0x0acd },
	{ CCI_REG16(0x0026), 0x0016 },
	{ CCI_REG16(0x002c), 0x07b1 },
	{ CCI_REG16(0x0128), 0x0002 },
	{ CCI_REG16(0x012a), 0x0000 },
	{ CCI_REG16(0x012c), 0x0a20 },
	{ CCI_REG16(0x012e), 0x0798 },
	{ CCI_REG16(0x0110), 0x0a20 },
	{ CCI_REG16(0x0112), 0x0798 },
	{ CCI_REG16(0x0006), 0x07c0 },
	{ CCI_REG16(0x0008), 0x0b40 },
	{ CCI_REG16(0x000a), 0x0db0 },
	{ CCI_REG16(0x0500), 0x0000 },
	{ CCI_REG16(0x0700), 0x0590 },
	{ CCI_REG16(0x001e), 0x0101 },
	{ CCI_REG16(0x0032), 0x0101 },
	{ CCI_REG16(0x0002), 0x0539 },
	{ CCI_REG16(0x0004), 0x07e0 },
	{ CCI_REG16(0x0a04), 0x011a },
};

static const struct cci_reg_sequence sr544_2592x1458_regs[] = {
	{ CCI_REG16(0x0b02), 0x0014 },
	{ CCI_REG16(0x0b04), 0x07c8 },
	{ CCI_REG16(0x0b06), 0x5ed7 },
	{ CCI_REG16(0x0b14), 0x5313 },
	{ CCI_REG16(0x0b16), 0x4a0b },
	{ CCI_REG16(0x0b18), 0x0000 },
	{ CCI_REG16(0x0b1a), 0x1044 },
	{ CCI_REG16(0x0012), 0x00aa },
	{ CCI_REG16(0x0018), 0x0acb },
	{ CCI_REG16(0x0026), 0x010c },
	{ CCI_REG16(0x002c), 0x06bd },
	{ CCI_REG16(0x0128), 0x0002 },
	{ CCI_REG16(0x012a), 0x0000 },
	{ CCI_REG16(0x012c), 0x0a20 },
	{ CCI_REG16(0x012e), 0x05b2 },
	{ CCI_REG16(0x0110), 0x0a20 },
	{ CCI_REG16(0x0112), 0x05b2 },
	{ CCI_REG16(0x0006), 0x07c0 },
	{ CCI_REG16(0x0008), 0x0b40 },
	{ CCI_REG16(0x000a), 0x0db0 },
	{ CCI_REG16(0x0700), 0x0590 },
	{ CCI_REG16(0x001e), 0x0101 },
	{ CCI_REG16(0x0032), 0x0101 },
	{ CCI_REG16(0x0a02), 0x0100 },
	{ CCI_REG16(0x0a04), 0x011a },
};

static const struct cci_reg_sequence sr544_648x488_regs[] = {
	{ CCI_REG16(0x0b02), 0x0014 },
	{ CCI_REG16(0x0b04), 0x07c8 },
	{ CCI_REG16(0x0b06), 0x5ed7 },
	{ CCI_REG16(0x0b14), 0x5313 },
	{ CCI_REG16(0x0b16), 0x4a0b },
	{ CCI_REG16(0x0b18), 0x0000 },
	{ CCI_REG16(0x0b1a), 0x1044 },
	{ CCI_REG16(0x0012), 0x00aa },
	{ CCI_REG16(0x0018), 0x0acd },
	{ CCI_REG16(0x0026), 0x0012 },
	{ CCI_REG16(0x002c), 0x07b1 },
	{ CCI_REG16(0x0128), 0x0002 },
	{ CCI_REG16(0x012a), 0x0000 },
	{ CCI_REG16(0x012c), 0x0510 },
	{ CCI_REG16(0x012e), 0x01e8 },
	{ CCI_REG16(0x0110), 0x0288 },
	{ CCI_REG16(0x0112), 0x01e8 },
	{ CCI_REG16(0x0006), 0x01f8 },
	{ CCI_REG16(0x0008), 0x0b40 },
	{ CCI_REG16(0x000a), 0x0db0 },
	{ CCI_REG16(0x0700), 0x215a },
	{ CCI_REG16(0x001e), 0x0301 },
	{ CCI_REG16(0x0032), 0x0701 },
	{ CCI_REG16(0x0a02), 0x0100 },
	{ CCI_REG16(0x0a04), 0x013a },
};

static const struct sr544_mode sr544_modes[] = {
	{
		.width = 2592,
		.height = 1944,
		.line_length = 2880,
		/* Preserve the validated 29.69 fps exposure-safe default. */
		.frame_length = SR544_EXPOSURE_DEFAULT + SR544_EXPOSURE_MARGIN,
		.reg_list = sr544_2592x1944_regs,
		.num_regs = ARRAY_SIZE(sr544_2592x1944_regs),
	},
	{
		.width = 2592,
		.height = 1458,
		.line_length = 2880,
		.frame_length = SR544_EXPOSURE_DEFAULT + SR544_EXPOSURE_MARGIN,
		.reg_list = sr544_2592x1458_regs,
		.num_regs = ARRAY_SIZE(sr544_2592x1458_regs),
	},
	{
		.width = 648,
		.height = 488,
		.line_length = 2880,
		.frame_length = 504,
		.reg_list = sr544_648x488_regs,
		.num_regs = ARRAY_SIZE(sr544_648x488_regs),
	},
};

static const struct cci_reg_sequence sr544_otp_mode_regs[] = {
	{ CCI_REG8(0x0f02), 0x00 },
	{ CCI_REG8(0x011a), 0x01 },
	{ CCI_REG8(0x011b), 0x09 },
	{ CCI_REG8(0x0d04), 0x01 },
	{ CCI_REG8(0x0d00), 0x07 },
	{ CCI_REG8(0x004c), 0x01 },
	{ CCI_REG8(0x003e), 0x01 },
};

static int sr544_otp_prepare_read(struct sr544 *sensor, u16 address)
{
	int ret = 0;

	/* Match Samsung's byte-wide OTP-mode transition and delays. */
	cci_write(sensor->regmap, CCI_REG8(0x0118), 0x00, &ret);
	if (ret)
		return ret;

	msleep(100);
	cci_multi_reg_write(sensor->regmap, sr544_otp_mode_regs,
			    ARRAY_SIZE(sr544_otp_mode_regs), &ret);
	cci_write(sensor->regmap, CCI_REG8(0x0118), 0x01, &ret);
	if (ret)
		return ret;

	msleep(100);
	cci_write(sensor->regmap, SR544_REG_OTP_ADDRESS_H, address >> 8,
		  &ret);
	cci_write(sensor->regmap, SR544_REG_OTP_ADDRESS_L, address & 0xff,
		  &ret);
	cci_write(sensor->regmap, SR544_REG_OTP_COMMAND,
		  SR544_OTP_COMMAND_READ, &ret);

	return ret;
}

static int sr544_load_otp(struct sr544 *sensor)
{
	struct device *dev = sensor->sd.dev;
	u16 start_address;
	u64 value;
	unsigned int i;
	int stop_ret = 0;
	int ret = 0;

	/* The OTP controller executes firmware uploaded through the sensor. */
	cci_multi_reg_write(sensor->regmap, sr544_init_regs,
			    ARRAY_SIZE(sr544_init_regs), &ret);
	cci_write(sensor->regmap, SR544_REG_MODE_SELECT,
		  SR544_MODE_STREAMING, &ret);
	if (ret)
		return ret;

	msleep(100);
	ret = sr544_otp_prepare_read(sensor, SR544_OTP_VERSION_ADDRESS);
	if (ret)
		goto stop_sensor;

	cci_read(sensor->regmap, SR544_REG_OTP_READ_DATA, &value, &ret);
	if (ret)
		goto stop_sensor;
	sensor->otp_version = value;

	ret = sr544_otp_prepare_read(sensor, SR544_OTP_BANK_SELECT_ADDRESS);
	if (ret)
		goto stop_sensor;

	cci_read(sensor->regmap, SR544_REG_OTP_READ_DATA, &value, &ret);
	if (ret)
		goto stop_sensor;
	sensor->otp_bank = value;

	switch (sensor->otp_bank) {
	case 0:
	case 1:
		start_address = SR544_OTP_BANK_0_START;
		break;
	case 3:
		start_address = SR544_OTP_BANK_1_START;
		break;
	case 7:
		start_address = SR544_OTP_BANK_2_START;
		break;
	default:
		ret = dev_err_probe(dev, -EINVAL,
				    "unsupported OTP bank marker 0x%02x\n",
				    sensor->otp_bank);
		goto stop_sensor;
	}

	cci_write(sensor->regmap, SR544_REG_OTP_ADDRESS_H,
		  start_address >> 8, &ret);
	cci_write(sensor->regmap, SR544_REG_OTP_ADDRESS_L,
		  start_address & 0xff, &ret);
	cci_write(sensor->regmap, SR544_REG_OTP_COMMAND,
		  SR544_OTP_COMMAND_READ, &ret);
	if (ret)
		goto stop_sensor;

	for (i = 0; i < SR544_OTP_SIZE; i++) {
		cci_read(sensor->regmap, SR544_REG_OTP_READ_DATA, &value,
			 &ret);
		if (ret)
			goto stop_sensor;
		sensor->otp_data[i] = value;
	}

	/* Preserve the layout exported by Samsung's downstream driver. */
	sensor->otp_data[SR544_OTP_VERSION_OFFSET] = sensor->otp_version;
	sensor->otp_cached = true;
	dev_info(dev, "cached 0x%x OTP bytes from bank 0x%02x (version 0x%02x)\n",
		 SR544_OTP_SIZE, sensor->otp_bank, sensor->otp_version);

stop_sensor:
	cci_write(sensor->regmap, SR544_REG_MODE_SELECT,
		  SR544_MODE_STANDBY, &stop_ret);
	if (stop_ret)
		dev_warn(dev, "failed to leave OTP mode: %d\n", stop_ret);

	return ret;
}

static int sr544_nvmem_read(void *priv, unsigned int offset, void *val,
			    size_t bytes)
{
	struct sr544 *sensor = priv;
	struct device *dev = sensor->sd.dev;
	struct v4l2_subdev_state *state;
	int ret = 0;

	if (offset >= SR544_OTP_SIZE || bytes > SR544_OTP_SIZE - offset)
		return -EINVAL;

	state = v4l2_subdev_lock_and_get_active_state(&sensor->sd);
	if (!sensor->otp_cached) {
		if (sensor->streaming) {
			ret = -EBUSY;
			goto unlock;
		}

		ret = pm_runtime_resume_and_get(dev);
		if (ret < 0)
			goto unlock;

		ret = sr544_load_otp(sensor);
		pm_runtime_mark_last_busy(dev);
		pm_runtime_put_autosuspend(dev);
		if (ret)
			goto unlock;
	}

	memcpy(val, sensor->otp_data + offset, bytes);

unlock:
	v4l2_subdev_unlock_state(state);
	return ret;
}

static int sr544_register_nvmem(struct sr544 *sensor)
{
	struct nvmem_config config = {
		.dev = sensor->sd.dev,
		.name = "sr544-otp",
		.id = NVMEM_DEVID_NONE,
		.owner = THIS_MODULE,
		.type = NVMEM_TYPE_OTP,
		.read_only = true,
		.root_only = true,
		.reg_read = sr544_nvmem_read,
		.priv = sensor,
		.size = SR544_OTP_SIZE,
		.word_size = 1,
		.stride = 1,
	};
	struct nvmem_device *nvmem;

	nvmem = devm_nvmem_register(sensor->sd.dev, &config);
	return PTR_ERR_OR_ZERO(nvmem);
}

static void sr544_fill_format(const struct sr544_mode *mode,
			      struct v4l2_mbus_framefmt *fmt)
{
	fmt->width = mode->width;
	fmt->height = mode->height;
	fmt->code = MEDIA_BUS_FMT_SBGGR10_1X10;
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_RAW;
	fmt->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	fmt->xfer_func = V4L2_XFER_FUNC_NONE;
}

static int sr544_enum_mbus_code(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *state,
				struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index)
		return -EINVAL;

	code->code = MEDIA_BUS_FMT_SBGGR10_1X10;
	return 0;
}

static int sr544_enum_frame_size(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index >= ARRAY_SIZE(sr544_modes) ||
	    fse->code != MEDIA_BUS_FMT_SBGGR10_1X10)
		return -EINVAL;

	fse->min_width = sr544_modes[fse->index].width;
	fse->max_width = sr544_modes[fse->index].width;
	fse->min_height = sr544_modes[fse->index].height;
	fse->max_height = sr544_modes[fse->index].height;
	return 0;
}

static int sr544_update_mode_controls(struct sr544 *sensor,
				      const struct sr544_mode *mode)
{
	s64 hblank = mode->line_length - mode->width;
	s64 vblank = mode->frame_length - mode->height;
	int ret;

	ret = __v4l2_ctrl_modify_range(sensor->vblank, vblank,
				       SR544_FRAME_LENGTH_MAX - mode->height,
				       1, vblank);
	if (ret)
		return ret;
	ret = __v4l2_ctrl_s_ctrl(sensor->vblank, vblank);
	if (ret)
		return ret;

	return __v4l2_ctrl_modify_range(sensor->hblank, hblank, hblank,
					1, hblank);
}

static int sr544_set_format(struct v4l2_subdev *sd,
			    struct v4l2_subdev_state *state,
			    struct v4l2_subdev_format *format)
{
	struct sr544 *sensor = to_sr544(sd);
	const struct sr544_mode *mode;
	struct v4l2_mbus_framefmt *fmt;
	int ret;

	mode = v4l2_find_nearest_size(sr544_modes, ARRAY_SIZE(sr544_modes),
				      width, height, format->format.width,
				      format->format.height);

	if (format->which == V4L2_SUBDEV_FORMAT_ACTIVE && sensor->streaming)
		return -EBUSY;

	sr544_fill_format(mode, &format->format);
	fmt = v4l2_subdev_state_get_format(state, format->pad);
	*fmt = format->format;

	if (format->which == V4L2_SUBDEV_FORMAT_TRY)
		return 0;

	sensor->cur_mode = mode;
	ret = sr544_update_mode_controls(sensor, mode);
	if (ret)
		return ret;

	return 0;
}

static int sr544_init_state(struct v4l2_subdev *sd,
			    struct v4l2_subdev_state *state)
{
	struct v4l2_subdev_format format = {
		.pad = 0,
		.which = V4L2_SUBDEV_FORMAT_TRY,
		.format = {
			.width = 2592,
			.height = 1944,
		},
	};

	return sr544_set_format(sd, state, &format);
}

static int sr544_start_streaming(struct sr544 *sensor)
{
	struct device *dev = sensor->sd.dev;
	int ret = 0;

	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0)
		return ret;

	cci_multi_reg_write(sensor->regmap, sr544_init_regs,
			    ARRAY_SIZE(sr544_init_regs), &ret);
	if (ret) {
		dev_err(dev, "failed to upload sensor firmware: %d\n", ret);
		goto power_down;
	}

	cci_multi_reg_write(sensor->regmap, sensor->cur_mode->reg_list,
			    sensor->cur_mode->num_regs, &ret);
	if (ret) {
		dev_err(dev, "failed to program %ux%u mode: %d\n",
			sensor->cur_mode->width, sensor->cur_mode->height, ret);
		goto power_down;
	}

	/* Restore all user controls after the mode table overwrote them. */
	ret = __v4l2_ctrl_handler_setup(&sensor->ctrls);
	if (ret) {
		dev_err(dev, "failed to apply controls: %d\n", ret);
		goto power_down;
	}

	cci_write(sensor->regmap, SR544_REG_MODE_SELECT,
		  SR544_MODE_STREAMING, &ret);
	if (ret) {
		dev_err(dev, "failed to start streaming: %d\n", ret);
		goto power_down;
	}

	/* Match the settling delay used by Samsung after the native setup. */
	msleep(100);
	return 0;

power_down:
	pm_runtime_put(dev);
	return ret;
}

static void sr544_stop_streaming(struct sr544 *sensor)
{
	struct device *dev = sensor->sd.dev;
	int ret = 0;

	cci_write(sensor->regmap, SR544_REG_MODE_SELECT,
		  SR544_MODE_STANDBY, &ret);
	if (ret)
		dev_warn(dev, "failed to stop streaming: %d\n", ret);

	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);
}

static int sr544_set_stream(struct v4l2_subdev *sd, int enable)
{
	struct sr544 *sensor = to_sr544(sd);
	struct v4l2_subdev_state *state;
	int ret = 0;

	state = v4l2_subdev_lock_and_get_active_state(sd);
	if (!!enable == sensor->streaming)
		goto unlock;

	if (enable)
		ret = sr544_start_streaming(sensor);
	else
		sr544_stop_streaming(sensor);

	if (!ret)
		sensor->streaming = enable;

unlock:
	v4l2_subdev_unlock_state(state);
	return ret;
}

static const struct v4l2_subdev_video_ops sr544_video_ops = {
	.s_stream = sr544_set_stream,
};

static const struct v4l2_subdev_pad_ops sr544_pad_ops = {
	.enum_mbus_code = sr544_enum_mbus_code,
	.enum_frame_size = sr544_enum_frame_size,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = sr544_set_format,
};

static const struct v4l2_subdev_ops sr544_subdev_ops = {
	.video = &sr544_video_ops,
	.pad = &sr544_pad_ops,
};

static const struct v4l2_subdev_internal_ops sr544_internal_ops = {
	.init_state = sr544_init_state,
};

static int sr544_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct sr544 *sensor = to_sr544(sd);
	int ret;

	gpiod_set_value_cansleep(sensor->standby_gpio, 0);
	gpiod_set_value_cansleep(sensor->reset_gpio, 0);
	usleep_range(1000, 1500);

	ret = regulator_enable(sensor->vio);
	if (ret)
		return ret;
	usleep_range(1000, 1500);

	ret = regulator_enable(sensor->vdig);
	if (ret)
		goto disable_vio;
	usleep_range(1000, 1500);

	ret = regulator_enable(sensor->vana);
	if (ret)
		goto disable_vdig;
	usleep_range(1000, 1500);

	gpiod_set_value_cansleep(sensor->standby_gpio, 1);
	usleep_range(1000, 1500);

	ret = clk_prepare_enable(sensor->xclk);
	if (ret)
		goto standby_low;

	/* Samsung's sequence holds reset low for 10 ms after MCLK starts. */
	usleep_range(10000, 10500);
	gpiod_set_value_cansleep(sensor->reset_gpio, 1);
	usleep_range(3000, 3500);

	return 0;

standby_low:
	gpiod_set_value_cansleep(sensor->standby_gpio, 0);
	regulator_disable(sensor->vana);
disable_vdig:
	regulator_disable(sensor->vdig);
disable_vio:
	regulator_disable(sensor->vio);
	return ret;
}

static int sr544_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct sr544 *sensor = to_sr544(sd);

	gpiod_set_value_cansleep(sensor->reset_gpio, 0);
	usleep_range(1000, 1500);
	clk_disable_unprepare(sensor->xclk);
	gpiod_set_value_cansleep(sensor->standby_gpio, 0);
	usleep_range(1000, 1500);
	regulator_disable(sensor->vana);
	regulator_disable(sensor->vdig);
	regulator_disable(sensor->vio);

	return 0;
}

static int sr544_identify(struct sr544 *sensor)
{
	struct device *dev = sensor->sd.dev;
	u64 chip_id;
	int ret = 0;

	cci_read(sensor->regmap, SR544_REG_CHIP_ID, &chip_id, &ret);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read chip ID\n");

	if (chip_id != SR544_CHIP_ID) {
		dev_err(dev, "chip ID mismatch: expected 0x%04x, got 0x%04llx\n",
			SR544_CHIP_ID, chip_id);
		return -ENODEV;
	}

	dev_info(dev, "SR544 detected (chip ID 0x%04llx)\n", chip_id);
	return 0;
}

static int sr544_check_hwcfg(struct sr544 *sensor)
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

	if (ep.bus.mipi_csi2.num_data_lanes != 2) {
		dev_err(dev, "exactly two CSI-2 data lanes are required\n");
		ret = -EINVAL;
		goto free_endpoint;
	}

	if (ep.nr_of_link_frequencies != 1) {
		dev_err(dev, "one link-frequency value is required\n");
		ret = -EINVAL;
		goto free_endpoint;
	}

	sensor->link_freq = ep.link_frequencies[0];
	if (sensor->link_freq != SR544_LINK_FREQ) {
		dev_err(dev, "unsupported link frequency %lld Hz\n",
			sensor->link_freq);
		ret = -EINVAL;
	}

free_endpoint:
	v4l2_fwnode_endpoint_free(&ep);
	return ret;
}

static int sr544_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct sr544 *sensor = container_of(ctrl->handler, struct sr544,
					    ctrls);
	struct device *dev = sensor->sd.dev;
	s64 exposure_max;
	int ret;

	/* Keep the exposure range inside the requested frame length. */
	if (ctrl->id == V4L2_CID_VBLANK) {
		exposure_max = sensor->cur_mode->height + ctrl->val -
			       SR544_EXPOSURE_MARGIN;
		__v4l2_ctrl_modify_range(sensor->exposure,
					 sensor->exposure->minimum,
					 exposure_max,
					 sensor->exposure->step,
					 min_t(s64, sensor->exposure->default_value,
					       exposure_max));
	}

	ret = pm_runtime_get_if_in_use(dev);
	if (ret <= 0)
		return ret < 0 ? ret : 0;

	ret = 0;
	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		cci_write(sensor->regmap, SR544_REG_EXPOSURE, ctrl->val, &ret);
		break;
	case V4L2_CID_ANALOGUE_GAIN:
		cci_write(sensor->regmap, SR544_REG_ANALOGUE_GAIN,
			  ctrl->val, &ret);
		break;
	case V4L2_CID_DIGITAL_GAIN:
		cci_write(sensor->regmap, SR544_REG_DIGITAL_GAIN_GR,
			  ctrl->val, &ret);
		cci_write(sensor->regmap, SR544_REG_DIGITAL_GAIN_GB,
			  ctrl->val, &ret);
		cci_write(sensor->regmap, SR544_REG_DIGITAL_GAIN_R,
			  ctrl->val, &ret);
		cci_write(sensor->regmap, SR544_REG_DIGITAL_GAIN_B,
			  ctrl->val, &ret);
		break;
	case V4L2_CID_VBLANK:
		cci_write(sensor->regmap, SR544_REG_FRAME_LENGTH,
			  sensor->cur_mode->height + ctrl->val, &ret);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	pm_runtime_put(dev);
	return ret;
}

static const struct v4l2_ctrl_ops sr544_ctrl_ops = {
	.s_ctrl = sr544_set_ctrl,
};

static int sr544_init_controls(struct sr544 *sensor)
{
	const struct sr544_mode *mode = sensor->cur_mode;
	struct v4l2_ctrl *ctrl;
	s64 exposure_max;
	s64 hblank;
	s64 vblank_default;
	int ret;

	ret = v4l2_ctrl_handler_init(&sensor->ctrls, 7);
	if (ret)
		return ret;

	ctrl = v4l2_ctrl_new_int_menu(&sensor->ctrls, &sr544_ctrl_ops,
				      V4L2_CID_LINK_FREQ, 0, 0,
				      &sensor->link_freq);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	ctrl = v4l2_ctrl_new_std(&sensor->ctrls, &sr544_ctrl_ops,
				 V4L2_CID_PIXEL_RATE,
				 SR544_PIXEL_RATE, SR544_PIXEL_RATE,
				 1, SR544_PIXEL_RATE);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	vblank_default = mode->frame_length - mode->height;
	sensor->vblank =
		v4l2_ctrl_new_std(&sensor->ctrls, &sr544_ctrl_ops,
				  V4L2_CID_VBLANK, vblank_default,
				  SR544_FRAME_LENGTH_MAX - mode->height, 1,
				  vblank_default);

	hblank = mode->line_length - mode->width;
	sensor->hblank =
		v4l2_ctrl_new_std(&sensor->ctrls, &sr544_ctrl_ops,
				  V4L2_CID_HBLANK, hblank, hblank, 1,
				  hblank);
	if (sensor->hblank)
		sensor->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	exposure_max = mode->frame_length - SR544_EXPOSURE_MARGIN;
	sensor->exposure = v4l2_ctrl_new_std(&sensor->ctrls, &sr544_ctrl_ops,
					     V4L2_CID_EXPOSURE,
					     SR544_EXPOSURE_MIN,
					     exposure_max, 1,
					     SR544_EXPOSURE_DEFAULT);
	v4l2_ctrl_new_std(&sensor->ctrls, &sr544_ctrl_ops,
			  V4L2_CID_ANALOGUE_GAIN,
			  SR544_ANALOGUE_GAIN_MIN, SR544_ANALOGUE_GAIN_MAX,
			  1, SR544_ANALOGUE_GAIN_MIN);
	v4l2_ctrl_new_std(&sensor->ctrls, &sr544_ctrl_ops,
			  V4L2_CID_DIGITAL_GAIN,
			  SR544_DIGITAL_GAIN_MIN, SR544_DIGITAL_GAIN_MAX,
			  1, SR544_DIGITAL_GAIN_MIN);

	if (sensor->ctrls.error) {
		ret = sensor->ctrls.error;
		v4l2_ctrl_handler_free(&sensor->ctrls);
		return ret;
	}

	sensor->sd.ctrl_handler = &sensor->ctrls;
	return 0;
}

static int sr544_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct sr544 *sensor;
	unsigned long xclk_rate;
	int ret;

	sensor = devm_kzalloc(dev, sizeof(*sensor), GFP_KERNEL);
	if (!sensor)
		return -ENOMEM;

	v4l2_i2c_subdev_init(&sensor->sd, client, &sr544_subdev_ops);
	sensor->sd.internal_ops = &sr544_internal_ops;

	ret = sr544_check_hwcfg(sensor);
	if (ret)
		return ret;

	sensor->xclk = devm_clk_get(dev, "xclk");
	if (IS_ERR(sensor->xclk))
		return dev_err_probe(dev, PTR_ERR(sensor->xclk),
				     "failed to get xclk\n");

	ret = clk_set_rate(sensor->xclk, SR544_XCLK_FREQ);
	if (ret)
		return dev_err_probe(dev, ret, "failed to set xclk rate\n");

	xclk_rate = clk_get_rate(sensor->xclk);
	if (xclk_rate < SR544_XCLK_MIN || xclk_rate > SR544_XCLK_MAX)
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

	sensor->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(sensor->regmap))
		return dev_err_probe(dev, PTR_ERR(sensor->regmap),
				     "failed to initialize CCI regmap\n");

	ret = sr544_power_on(dev);
	if (ret)
		return dev_err_probe(dev, ret, "failed to power on sensor\n");

	ret = sr544_identify(sensor);
	if (ret)
		goto power_off;

	sensor->cur_mode = &sr544_modes[0];
	ret = sr544_init_controls(sensor);
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

	ret = sr544_register_nvmem(sensor);
	if (ret)
		goto cleanup_subdev;

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
	sr544_power_off(dev);
	return ret;
}

static void sr544_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct sr544 *sensor = to_sr544(sd);
	struct device *dev = &client->dev;

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&sensor->ctrls);

	pm_runtime_disable(dev);
	if (!pm_runtime_status_suspended(dev))
		sr544_power_off(dev);
	pm_runtime_set_suspended(dev);
}

static const struct dev_pm_ops sr544_pm_ops = {
	RUNTIME_PM_OPS(sr544_power_off, sr544_power_on, NULL)
};

static const struct of_device_id sr544_of_match[] = {
	{ .compatible = "siliconfile,sr544" },
	{ }
};
MODULE_DEVICE_TABLE(of, sr544_of_match);

static struct i2c_driver sr544_i2c_driver = {
	.driver = {
		.name = "sr544",
		.of_match_table = sr544_of_match,
		.pm = pm_ptr(&sr544_pm_ops),
	},
	.probe = sr544_probe,
	.remove = sr544_remove,
};
module_i2c_driver(sr544_i2c_driver);

MODULE_DESCRIPTION("SiliconFile SR544 camera sensor driver");
MODULE_LICENSE("GPL");
