// SPDX-License-Identifier: GPL-2.0-only
/*
 * Backlight driver for the Richtek RT8555
 *
 * Copyright (C) 2022 RDS5
 * Copyright (C) 2026 postmarketOS contributors
 */

#include <linux/backlight.h>
#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/regmap.h>

#define RT8555_MAX_BRIGHTNESS			1023
#define RT8555_DEFAULT_BRIGHTNESS		511

#define RT8555_MIN_LED_CURRENT_UA		10020
#define RT8555_DEFAULT_LED_CURRENT_UA		20040
#define RT8555_MAX_LED_CURRENT_UA		35000
#define RT8555_MIN_LED_CURRENT_CODE		0x49
#define RT8555_MAX_LED_CURRENT_CODE		0xff

#define RT8555_REG_CONFIG0			0x00
#define RT8555_FIXED_26KHZ			BIT(7)
#define RT8555_MIXED_MODE_THRESHOLD_MASK	GENMASK(3, 2)
#define RT8555_BRIGHTNESS_SOURCE_I2C		BIT(1)
#define RT8555_DIMMING_MODE_MIXED		BIT(0)

#define RT8555_REG_CONFIG1			0x01
#define RT8555_10BIT_BRIGHTNESS			BIT(7)

#define RT8555_REG_LED_CURRENT			0x02

#define RT8555_REG_BRIGHTNESS_LSB		0x04
#define RT8555_REG_BRIGHTNESS_MSB		0x05
#define RT8555_BRIGHTNESS_MSB_MASK		GENMASK(1, 0)

#define RT8555_REG_CONFIG8			0x08
#define RT8555_LED_HEADROOM_MASK		GENMASK(3, 2)

#define RT8555_REG_MAX				0x0e

struct rt8555 {
	struct device *dev;
	struct regmap *regmap;
	struct backlight_device *backlight;
	struct gpio_desc *enable_gpio;
	u8 led_current;
	u8 led_headroom;
	bool enabled;
};

static int rt8555_apply_config(struct rt8555 *rt)
{
	int ret;

	/*
	 * Select 10-bit I2C brightness control and DC dimming. The data sheet
	 * defines DC mode as mixed mode with a zero-percent PWM threshold.
	 */
	ret = regmap_update_bits(rt->regmap, RT8555_REG_CONFIG0,
				 RT8555_FIXED_26KHZ |
				 RT8555_MIXED_MODE_THRESHOLD_MASK |
				 RT8555_BRIGHTNESS_SOURCE_I2C |
				 RT8555_DIMMING_MODE_MIXED,
				 RT8555_FIXED_26KHZ |
				 RT8555_BRIGHTNESS_SOURCE_I2C |
				 RT8555_DIMMING_MODE_MIXED);
	if (ret)
		return ret;

	ret = regmap_update_bits(rt->regmap, RT8555_REG_CONFIG1,
				 RT8555_10BIT_BRIGHTNESS,
				 RT8555_10BIT_BRIGHTNESS);
	if (ret)
		return ret;

	ret = regmap_write(rt->regmap, RT8555_REG_LED_CURRENT,
			   rt->led_current);
	if (ret)
		return ret;

	return regmap_update_bits(rt->regmap, RT8555_REG_CONFIG8,
				  RT8555_LED_HEADROOM_MASK,
				  FIELD_PREP(RT8555_LED_HEADROOM_MASK,
					     rt->led_headroom));
}

static int rt8555_enable(struct rt8555 *rt)
{
	int ret;

	if (rt->enabled)
		return 0;

	if (rt->enable_gpio) {
		gpiod_set_value_cansleep(rt->enable_gpio, 1);
		usleep_range(10000, 20000);
	}

	ret = rt8555_apply_config(rt);
	if (ret) {
		if (rt->enable_gpio)
			gpiod_set_value_cansleep(rt->enable_gpio, 0);
		return ret;
	}

	rt->enabled = true;
	return 0;
}

static void rt8555_disable(struct rt8555 *rt)
{
	if (!rt->enable_gpio)
		return;

	gpiod_set_value_cansleep(rt->enable_gpio, 0);
	rt->enabled = false;
}

static int rt8555_write_brightness(struct rt8555 *rt,
				   unsigned int brightness)
{
	u8 buf[] = {
		brightness & 0xff,
		(brightness >> 8) & RT8555_BRIGHTNESS_MSB_MASK,
	};

	/* The data sheet requires a series write for 10-bit brightness. */
	return regmap_raw_write(rt->regmap, RT8555_REG_BRIGHTNESS_LSB,
				buf, sizeof(buf));
}

static int rt8555_update_status(struct backlight_device *backlight)
{
	struct rt8555 *rt = bl_get_data(backlight);
	unsigned int brightness = backlight_get_brightness(backlight);
	int ret;

	if (!brightness) {
		/* A tied-high device still needs an explicit zero written. */
		if (!rt->enabled && !rt->enable_gpio) {
			ret = rt8555_enable(rt);
			if (ret)
				return ret;
		}

		if (rt->enabled) {
			ret = rt8555_write_brightness(rt, 0);
			if (ret)
				return ret;
		}

		rt8555_disable(rt);
		return 0;
	}

	ret = rt8555_enable(rt);
	if (ret)
		return ret;

	return rt8555_write_brightness(rt, brightness);
}

static int rt8555_get_brightness(struct backlight_device *backlight)
{
	struct rt8555 *rt = bl_get_data(backlight);
	u8 buf[2];
	int ret;

	if (!rt->enabled)
		return 0;

	ret = regmap_raw_read(rt->regmap, RT8555_REG_BRIGHTNESS_LSB,
			      buf, sizeof(buf));
	if (ret)
		return ret;

	return buf[0] | ((buf[1] & RT8555_BRIGHTNESS_MSB_MASK) << 8);
}

static const struct backlight_ops rt8555_backlight_ops = {
	.options = BL_CORE_SUSPENDRESUME,
	.update_status = rt8555_update_status,
	.get_brightness = rt8555_get_brightness,
};

static u8 rt8555_led_current_to_code(u32 current_ua)
{
	return RT8555_MIN_LED_CURRENT_CODE +
		DIV_ROUND_CLOSEST((current_ua - RT8555_MIN_LED_CURRENT_UA) *
				  (RT8555_MAX_LED_CURRENT_CODE -
				   RT8555_MIN_LED_CURRENT_CODE),
				  RT8555_MAX_LED_CURRENT_UA -
				  RT8555_MIN_LED_CURRENT_UA);
}

static int rt8555_parse_properties(struct rt8555 *rt,
				   struct backlight_properties *props)
{
	static const u32 headroom_uv[] = { 500000, 570000, 600000, 700000 };
	u32 value = RT8555_DEFAULT_LED_CURRENT_UA;
	unsigned int i;

	device_property_read_u32(rt->dev, "richtek,led-current-microamp",
				 &value);
	if (value < RT8555_MIN_LED_CURRENT_UA ||
	    value > RT8555_MAX_LED_CURRENT_UA)
		return dev_err_probe(rt->dev, -EINVAL,
				     "invalid LED current %u uA\n", value);
	rt->led_current = rt8555_led_current_to_code(value);

	value = headroom_uv[0];
	device_property_read_u32(rt->dev,
				 "richtek,led-driver-headroom-microvolt",
				 &value);
	for (i = 0; i < ARRAY_SIZE(headroom_uv); i++)
		if (value == headroom_uv[i])
			break;
	if (i == ARRAY_SIZE(headroom_uv))
		return dev_err_probe(rt->dev, -EINVAL,
				     "invalid LED driver headroom %u uV\n",
				     value);
	rt->led_headroom = i;

	value = RT8555_MAX_BRIGHTNESS;
	device_property_read_u32(rt->dev, "max-brightness", &value);
	if (!value || value > RT8555_MAX_BRIGHTNESS)
		return dev_err_probe(rt->dev, -EINVAL,
				     "invalid maximum brightness %u\n", value);
	props->max_brightness = value;

	value = min_t(u32, RT8555_DEFAULT_BRIGHTNESS,
		      props->max_brightness);
	device_property_read_u32(rt->dev, "default-brightness", &value);
	if (value > props->max_brightness)
		return dev_err_probe(rt->dev, -EINVAL,
				     "default brightness %u exceeds maximum %u\n",
				     value, props->max_brightness);
	props->brightness = value;

	return 0;
}

static const struct regmap_config rt8555_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = RT8555_REG_MAX,
};

static int rt8555_probe(struct i2c_client *client)
{
	struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.scale = BACKLIGHT_SCALE_LINEAR,
	};
	struct device *dev = &client->dev;
	struct rt8555 *rt;
	int ret;

	rt = devm_kzalloc(dev, sizeof(*rt), GFP_KERNEL);
	if (!rt)
		return -ENOMEM;

	rt->dev = dev;
	rt->enable_gpio = devm_gpiod_get_optional(dev, "enable",
						  GPIOD_OUT_LOW);
	if (IS_ERR(rt->enable_gpio))
		return dev_err_probe(dev, PTR_ERR(rt->enable_gpio),
				     "failed to get enable GPIO\n");

	rt->regmap = devm_regmap_init_i2c(client, &rt8555_regmap_config);
	if (IS_ERR(rt->regmap))
		return dev_err_probe(dev, PTR_ERR(rt->regmap),
				     "failed to initialize register map\n");

	ret = rt8555_parse_properties(rt, &props);
	if (ret)
		return ret;

	rt->backlight = devm_backlight_device_register(dev, dev_name(dev), dev,
						       rt, &rt8555_backlight_ops,
						       &props);
	if (IS_ERR(rt->backlight))
		return dev_err_probe(dev, PTR_ERR(rt->backlight),
				     "failed to register backlight\n");

	i2c_set_clientdata(client, rt);

	ret = backlight_update_status(rt->backlight);
	if (ret) {
		rt8555_disable(rt);
		return dev_err_probe(dev, ret,
				     "failed to initialize backlight\n");
	}

	return 0;
}

static void rt8555_remove(struct i2c_client *client)
{
	struct rt8555 *rt = i2c_get_clientdata(client);
	int ret;

	rt->backlight->props.brightness = 0;
	ret = backlight_update_status(rt->backlight);
	if (ret)
		dev_warn(rt->dev, "failed to turn backlight off: %d\n", ret);

	rt8555_disable(rt);
}

static void rt8555_shutdown(struct i2c_client *client)
{
	rt8555_remove(client);
}

static const struct of_device_id rt8555_of_match[] = {
	{ .compatible = "richtek,rt8555" },
	{ }
};
MODULE_DEVICE_TABLE(of, rt8555_of_match);

static const struct i2c_device_id rt8555_i2c_ids[] = {
	{ "rt8555" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, rt8555_i2c_ids);

static struct i2c_driver rt8555_driver = {
	.driver = {
		.name = "rt8555-backlight",
		.of_match_table = rt8555_of_match,
	},
	.probe = rt8555_probe,
	.remove = rt8555_remove,
	.shutdown = rt8555_shutdown,
	.id_table = rt8555_i2c_ids,
};
module_i2c_driver(rt8555_driver);

MODULE_AUTHOR("RDS5");
MODULE_DESCRIPTION("Richtek RT8555 backlight driver");
MODULE_LICENSE("GPL");
