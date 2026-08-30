// SPDX-License-Identifier: GPL-2.0-only
/*
 * V4L2 driver for the Samsung S5K4ECGX image sensor.
 *
 * This implements the native 1280x960 processed-YUV preview path used by
 * Samsung's SM-T560NU firmware. The sensor contains its own ISP, AE, AWB and
 * autofocus engines; CAMSS receives packed UYVY over two CSI-2 data lanes.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <linux/sched.h>

#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-mediabus.h>
#include <media/v4l2-subdev.h>

#define S5K4ECGX_CMD_READ_ADDR_H	CCI_REG16(0x002c)
#define S5K4ECGX_CMD_READ_ADDR_L	CCI_REG16(0x002e)
#define S5K4ECGX_CMD_READ_DATA		CCI_REG16(0x0f12)
#define S5K4ECGX_FW_VERSION_REG		0x700001a4
#define S5K4ECGX_REVISION_REG		0x700001a6
#define S5K4ECGX_FW_FAMILY		0x4e00
#define S5K4ECGX_FW_FAMILY_MASK		0xff00

#define S5K4ECGX_XCLK_FREQ		26000000
#define S5K4ECGX_XCLK_MIN		25900000
#define S5K4ECGX_XCLK_MAX		26100000
#define S5K4ECGX_LINK_FREQ		144000000
#define S5K4ECGX_PIXEL_RATE		36000000
#define S5K4ECGX_WIDTH			1280
#define S5K4ECGX_HEIGHT			960

#define S5K4ECGX_DATA_PORT		0x0f12
#define S5K4ECGX_DELAY			0xffff
#define S5K4ECGX_BURST_WORDS		4

struct s5k4ecgx_reg {
	u16 addr;
	u16 val;
};

#include "s5k4ecgx-gte.h"

struct s5k4ecgx {
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct v4l2_ctrl_handler ctrls;
	struct regmap *regmap;
	struct i2c_client *client;

	struct clk *xclk;
	struct regulator *vdig;
	struct regulator *vana;
	struct regulator *vio;
	struct regulator *vaf;
	struct gpio_desc *reset_gpio;
	struct gpio_desc *standby_gpio;

	s64 link_freq;
	bool streaming;
};

static inline struct s5k4ecgx *to_s5k4ecgx(struct v4l2_subdev *sd)
{
	return container_of(sd, struct s5k4ecgx, sd);
}

static int s5k4ecgx_burst_write(struct s5k4ecgx *sensor,
				const struct s5k4ecgx_reg *regs,
				unsigned int count)
{
	u8 data[2 + S5K4ECGX_BURST_WORDS * 2] = {
		S5K4ECGX_DATA_PORT >> 8,
		S5K4ECGX_DATA_PORT & 0xff,
	};
	unsigned int i;
	int ret;

	for (i = 0; i < count; i++) {
		data[2 + i * 2] = regs[i].val >> 8;
		data[3 + i * 2] = regs[i].val & 0xff;
	}

	ret = i2c_master_send(sensor->client, data, 2 + count * 2);
	if (ret == 2 + count * 2)
		return 0;

	return ret < 0 ? ret : -EIO;
}

static int s5k4ecgx_write_regs(struct s5k4ecgx *sensor,
			       const struct s5k4ecgx_reg *regs,
			       unsigned int count)
{
	struct device *dev = sensor->sd.dev;
	unsigned int burst_count;
	unsigned int i = 0;
	int ret;

	while (i < count) {
		if (regs[i].addr == S5K4ECGX_DELAY) {
			msleep(regs[i].val);
			i++;
			continue;
		}

		burst_count = 1;
		if (regs[i].addr == S5K4ECGX_DATA_PORT) {
			while (i + burst_count < count &&
			       burst_count < S5K4ECGX_BURST_WORDS &&
			       regs[i + burst_count].addr == S5K4ECGX_DATA_PORT)
				burst_count++;
		}

		if (burst_count > 1)
			ret = s5k4ecgx_burst_write(sensor, &regs[i],
						   burst_count);
		else
			ret = cci_write(sensor->regmap,
					CCI_REG16(regs[i].addr), regs[i].val,
					NULL);
		if (ret) {
			dev_err(dev, "register write failed at %u (0x%04x): %d\n",
				i, regs[i].addr, ret);
			return ret;
		}

		i += burst_count;
		if (!(i & 0xff))
			cond_resched();
	}

	return 0;
}

static void s5k4ecgx_fill_format(struct v4l2_mbus_framefmt *fmt)
{
	fmt->width = S5K4ECGX_WIDTH;
	fmt->height = S5K4ECGX_HEIGHT;
	fmt->code = MEDIA_BUS_FMT_UYVY8_1X16;
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_SRGB;
	fmt->ycbcr_enc = V4L2_YCBCR_ENC_601;
	fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	fmt->xfer_func = V4L2_XFER_FUNC_SRGB;
}

static int s5k4ecgx_enum_mbus_code(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *state,
				   struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index)
		return -EINVAL;

	code->code = MEDIA_BUS_FMT_UYVY8_1X16;
	return 0;
}

static int s5k4ecgx_enum_frame_size(struct v4l2_subdev *sd,
				    struct v4l2_subdev_state *state,
				    struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index || fse->code != MEDIA_BUS_FMT_UYVY8_1X16)
		return -EINVAL;

	fse->min_width = S5K4ECGX_WIDTH;
	fse->max_width = S5K4ECGX_WIDTH;
	fse->min_height = S5K4ECGX_HEIGHT;
	fse->max_height = S5K4ECGX_HEIGHT;
	return 0;
}

static int s5k4ecgx_set_format(struct v4l2_subdev *sd,
			       struct v4l2_subdev_state *state,
			       struct v4l2_subdev_format *format)
{
	struct v4l2_mbus_framefmt *fmt;

	s5k4ecgx_fill_format(&format->format);
	fmt = v4l2_subdev_state_get_format(state, format->pad);
	*fmt = format->format;

	return 0;
}

static int s5k4ecgx_get_selection(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_selection *sel)
{
	if (sel->pad)
		return -EINVAL;

	switch (sel->target) {
	case V4L2_SEL_TGT_NATIVE_SIZE:
		sel->r = (struct v4l2_rect) {
			.width = 2576,
			.height = 1932,
		};
		return 0;
	case V4L2_SEL_TGT_CROP_BOUNDS:
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP:
		sel->r = (struct v4l2_rect) {
			.width = S5K4ECGX_WIDTH,
			.height = S5K4ECGX_HEIGHT,
		};
		return 0;
	default:
		return -EINVAL;
	}
}

static int s5k4ecgx_init_state(struct v4l2_subdev *sd,
			       struct v4l2_subdev_state *state)
{
	struct v4l2_subdev_format format = {
		.pad = 0,
		.which = V4L2_SUBDEV_FORMAT_TRY,
	};

	return s5k4ecgx_set_format(sd, state, &format);
}

static int s5k4ecgx_start_streaming(struct s5k4ecgx *sensor)
{
	struct device *dev = sensor->sd.dev;
	int ret;

	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0)
		return ret;

	ret = s5k4ecgx_write_regs(sensor, s5k4ecgx_gte_init,
				  ARRAY_SIZE(s5k4ecgx_gte_init));
	if (ret)
		goto power_down;

	ret = s5k4ecgx_write_regs(sensor, s5k4ecgx_gte_1280x960,
				  ARRAY_SIZE(s5k4ecgx_gte_1280x960));
	if (ret)
		goto power_down;

	ret = s5k4ecgx_write_regs(sensor, s5k4ecgx_gte_preview,
				  ARRAY_SIZE(s5k4ecgx_gte_preview));
	if (ret)
		goto power_down;

	ret = s5k4ecgx_write_regs(sensor, s5k4ecgx_gte_anti_banding_60hz,
				  ARRAY_SIZE(s5k4ecgx_gte_anti_banding_60hz));
	if (ret)
		goto power_down;

	/* Samsung's preview transition table specifies a 150 ms settle time. */
	msleep(150);
	return 0;

power_down:
	pm_runtime_put(dev);
	return ret;
}

static void s5k4ecgx_stop_streaming(struct s5k4ecgx *sensor)
{
	struct device *dev = sensor->sd.dev;
	int ret;

	ret = s5k4ecgx_write_regs(sensor, s5k4ecgx_gte_stop,
				  ARRAY_SIZE(s5k4ecgx_gte_stop));
	if (ret)
		dev_warn(dev, "failed to stop preview: %d\n", ret);

	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);
}

static int s5k4ecgx_set_stream(struct v4l2_subdev *sd, int enable)
{
	struct s5k4ecgx *sensor = to_s5k4ecgx(sd);
	struct v4l2_subdev_state *state;
	int ret = 0;

	state = v4l2_subdev_lock_and_get_active_state(sd);
	if (!!enable == sensor->streaming)
		goto unlock;

	if (enable)
		ret = s5k4ecgx_start_streaming(sensor);
	else
		s5k4ecgx_stop_streaming(sensor);

	if (!ret)
		sensor->streaming = enable;

unlock:
	v4l2_subdev_unlock_state(state);
	return ret;
}

static const struct v4l2_subdev_video_ops s5k4ecgx_video_ops = {
	.s_stream = s5k4ecgx_set_stream,
};

static const struct v4l2_subdev_pad_ops s5k4ecgx_pad_ops = {
	.enum_mbus_code = s5k4ecgx_enum_mbus_code,
	.enum_frame_size = s5k4ecgx_enum_frame_size,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = s5k4ecgx_set_format,
	.get_selection = s5k4ecgx_get_selection,
};

static const struct v4l2_subdev_ops s5k4ecgx_subdev_ops = {
	.video = &s5k4ecgx_video_ops,
	.pad = &s5k4ecgx_pad_ops,
};

static const struct v4l2_subdev_internal_ops s5k4ecgx_internal_ops = {
	.init_state = s5k4ecgx_init_state,
};

static int s5k4ecgx_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct s5k4ecgx *sensor = to_s5k4ecgx(sd);
	int ret;

	gpiod_set_value_cansleep(sensor->reset_gpio, 0);
	gpiod_set_value_cansleep(sensor->standby_gpio, 0);

	ret = regulator_enable(sensor->vio);
	if (ret)
		return ret;

	ret = regulator_enable(sensor->vana);
	if (ret)
		goto disable_vio;
	usleep_range(1000, 1500);

	ret = clk_prepare_enable(sensor->xclk);
	if (ret)
		goto disable_vana;
	usleep_range(5000, 5500);

	ret = regulator_enable(sensor->vdig);
	if (ret)
		goto disable_xclk;
	usleep_range(2000, 2500);

	gpiod_set_value_cansleep(sensor->standby_gpio, 1);
	usleep_range(1000, 1500);
	gpiod_set_value_cansleep(sensor->reset_gpio, 1);
	usleep_range(1000, 1500);

	ret = regulator_enable(sensor->vaf);
	if (ret)
		goto reset_low;
	usleep_range(1000, 1500);

	return 0;

reset_low:
	gpiod_set_value_cansleep(sensor->reset_gpio, 0);
	gpiod_set_value_cansleep(sensor->standby_gpio, 0);
	regulator_disable(sensor->vdig);
disable_xclk:
	clk_disable_unprepare(sensor->xclk);
disable_vana:
	regulator_disable(sensor->vana);
disable_vio:
	regulator_disable(sensor->vio);
	return ret;
}

static int s5k4ecgx_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct s5k4ecgx *sensor = to_s5k4ecgx(sd);

	regulator_disable(sensor->vaf);
	usleep_range(10000, 10500);
	gpiod_set_value_cansleep(sensor->reset_gpio, 0);
	usleep_range(1000, 1500);
	clk_disable_unprepare(sensor->xclk);
	usleep_range(2000, 2500);
	gpiod_set_value_cansleep(sensor->standby_gpio, 0);
	usleep_range(1000, 1500);
	regulator_disable(sensor->vana);
	usleep_range(8000, 8500);
	regulator_disable(sensor->vio);
	usleep_range(1000, 1500);
	regulator_disable(sensor->vdig);
	usleep_range(1000, 1500);

	return 0;
}

static int s5k4ecgx_identify(struct s5k4ecgx *sensor)
{
	struct device *dev = sensor->sd.dev;
	u64 fw_version, revision;
	int ret = 0;

	cci_write(sensor->regmap, S5K4ECGX_CMD_READ_ADDR_H,
		  S5K4ECGX_FW_VERSION_REG >> 16, &ret);
	cci_write(sensor->regmap, S5K4ECGX_CMD_READ_ADDR_L,
		  S5K4ECGX_FW_VERSION_REG & 0xffff, &ret);
	cci_read(sensor->regmap, S5K4ECGX_CMD_READ_DATA, &fw_version, &ret);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read firmware version\n");

	if ((fw_version & S5K4ECGX_FW_FAMILY_MASK) != S5K4ECGX_FW_FAMILY) {
		dev_err(dev, "firmware mismatch: expected 0x%04x family, got 0x%04llx\n",
			S5K4ECGX_FW_FAMILY, fw_version);
		return -ENODEV;
	}

	cci_write(sensor->regmap, S5K4ECGX_CMD_READ_ADDR_H,
		  S5K4ECGX_REVISION_REG >> 16, &ret);
	cci_write(sensor->regmap, S5K4ECGX_CMD_READ_ADDR_L,
		  S5K4ECGX_REVISION_REG & 0xffff, &ret);
	cci_read(sensor->regmap, S5K4ECGX_CMD_READ_DATA, &revision, &ret);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read firmware revision\n");

	dev_info(dev, "S5K4ECGX detected (firmware 0x%04llx, revision 0x%04llx)\n",
		 fw_version, revision);
	return 0;
}

static int s5k4ecgx_check_hwcfg(struct s5k4ecgx *sensor)
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
	if (sensor->link_freq != S5K4ECGX_LINK_FREQ) {
		dev_err(dev, "unsupported link frequency %lld Hz\n",
			sensor->link_freq);
		ret = -EINVAL;
	}

free_endpoint:
	v4l2_fwnode_endpoint_free(&ep);
	return ret;
}

static int s5k4ecgx_init_controls(struct s5k4ecgx *sensor)
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
				 S5K4ECGX_PIXEL_RATE, S5K4ECGX_PIXEL_RATE,
				 1, S5K4ECGX_PIXEL_RATE);
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

static int s5k4ecgx_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct s5k4ecgx *sensor;
	unsigned long xclk_rate;
	int ret;

	sensor = devm_kzalloc(dev, sizeof(*sensor), GFP_KERNEL);
	if (!sensor)
		return -ENOMEM;

	sensor->client = client;
	v4l2_i2c_subdev_init(&sensor->sd, client, &s5k4ecgx_subdev_ops);
	sensor->sd.internal_ops = &s5k4ecgx_internal_ops;

	ret = s5k4ecgx_check_hwcfg(sensor);
	if (ret)
		return ret;

	sensor->xclk = devm_clk_get(dev, "xclk");
	if (IS_ERR(sensor->xclk))
		return dev_err_probe(dev, PTR_ERR(sensor->xclk),
				     "failed to get xclk\n");

	ret = clk_set_rate(sensor->xclk, S5K4ECGX_XCLK_FREQ);
	if (ret)
		return dev_err_probe(dev, ret, "failed to set xclk rate\n");

	xclk_rate = clk_get_rate(sensor->xclk);
	if (xclk_rate < S5K4ECGX_XCLK_MIN || xclk_rate > S5K4ECGX_XCLK_MAX)
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
	sensor->vaf = devm_regulator_get(dev, "vaf");
	if (IS_ERR(sensor->vaf))
		return dev_err_probe(dev, PTR_ERR(sensor->vaf),
				     "failed to get vaf supply\n");

	ret = regulator_set_voltage(sensor->vaf, 2800000, 2800000);
	if (ret)
		return dev_err_probe(dev, ret, "failed to set vaf voltage\n");

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

	ret = s5k4ecgx_power_on(dev);
	if (ret)
		return dev_err_probe(dev, ret, "failed to power on sensor\n");

	ret = s5k4ecgx_identify(sensor);
	if (ret)
		goto power_off;

	ret = s5k4ecgx_init_controls(sensor);
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
	s5k4ecgx_power_off(dev);
	return ret;
}

static void s5k4ecgx_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct s5k4ecgx *sensor = to_s5k4ecgx(sd);
	struct device *dev = &client->dev;

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&sensor->ctrls);

	pm_runtime_disable(dev);
	if (!pm_runtime_status_suspended(dev))
		s5k4ecgx_power_off(dev);
	pm_runtime_set_suspended(dev);
}

static const struct dev_pm_ops s5k4ecgx_pm_ops = {
	RUNTIME_PM_OPS(s5k4ecgx_power_off, s5k4ecgx_power_on, NULL)
};

static const struct of_device_id s5k4ecgx_of_match[] = {
	{ .compatible = "samsung,s5k4ecgx" },
	{ }
};
MODULE_DEVICE_TABLE(of, s5k4ecgx_of_match);

static struct i2c_driver s5k4ecgx_i2c_driver = {
	.driver = {
		.name = "s5k4ecgx",
		.of_match_table = s5k4ecgx_of_match,
		.pm = pm_ptr(&s5k4ecgx_pm_ops),
	},
	.probe = s5k4ecgx_probe,
	.remove = s5k4ecgx_remove,
};
module_i2c_driver(s5k4ecgx_i2c_driver);

MODULE_DESCRIPTION("Samsung S5K4ECGX camera sensor driver");
MODULE_LICENSE("GPL");
