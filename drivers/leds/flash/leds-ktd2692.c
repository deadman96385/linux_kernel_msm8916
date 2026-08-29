// SPDX-License-Identifier: GPL-2.0-only
/*
 * LED driver : leds-ktd2692.c
 *
 * Copyright (C) 2015 Samsung Electronics
 * Ingi Kim <ingi2.kim@samsung.com>
 */

#include <linux/cleanup.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/leds-expresswire.h>
#include <linux/led-class-flash.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/workqueue.h>

/* Value related the movie mode */
#define KTD2692_MOVIE_MODE_CURRENT_LEVELS	16
#define KTD2692_MM_TO_FL_RATIO(x)		((x) / 3)

/* Value related the flash mode */
#define KTD2692_FLASH_MODE_CURRENT_LEVELS	16
#define KTD2692_FLASH_MODE_DEFAULT_LEVEL	8
#define KTD2692_FLASH_MODE_TIMEOUT_LEVELS	8
#define KTD2692_FLASH_MODE_TIMEOUT_DISABLE	0
#define KTD2692_FLASH_MODE_TIMEOUT_STEP_US	262000
#define KTD2692_FLASH_MODE_TIMEOUT_MAX_US	1835000

/* Macro for getting offset of flash timeout */
#define GET_TIMEOUT_OFFSET(timeout, step)	((timeout) / (step))

/* Base register address */
#define KTD2692_REG_LVP_BASE			0x00
#define KTD2692_REG_FLASH_TIMEOUT_BASE		0x20
#define KTD2692_REG_MM_MIN_CURR_THRESHOLD_BASE	0x40
#define KTD2692_REG_MOVIE_CURRENT_BASE		0x60
#define KTD2692_REG_FLASH_CURRENT_BASE		0x80
#define KTD2692_REG_MODE_BASE			0xA0

/* KTD2692 default length of name */
#define KTD2692_NAME_LENGTH			20

/* Movie / Flash Mode Control */
enum ktd2692_led_mode {
	KTD2692_MODE_DISABLE = 0,	/* default */
	KTD2692_MODE_MOVIE,
	KTD2692_MODE_FLASH,
};

struct ktd2692_led_config_data {
	/* maximum LED current in movie mode */
	u32 movie_max_microamp;
	/* maximum LED current in flash mode */
	u32 flash_max_microamp;
	/* maximum flash timeout */
	u32 flash_max_timeout;
	/* max LED brightness level */
	enum led_brightness max_brightness;
};

static const struct expresswire_timing ktd2692_timing = {
	.poweroff_us = 700,
	.data_start_us = 10,
	.end_of_data_low_us = 10,
	.end_of_data_high_us = 350,
	.short_bitset_us = 4,
	.long_bitset_us = 12
};

static const u32 ktd2692_flash_timeout_us[KTD2692_FLASH_MODE_TIMEOUT_LEVELS] = {
	0, 262000, 524000, 786000, 1049000, 1311000, 1573000, 1835000,
};

struct ktd2692_context {
	struct device *dev;

	/* Common ExpressWire properties (ctrl GPIO and timing) */
	struct expresswire_common_props props;

	/* Related LED Flash class device */
	struct led_classdev_flash fled_cdev;

	/* secures access to the device */
	struct mutex lock;
	struct regulator *regulator;

	struct gpio_desc *aux_gpio;
	struct delayed_work flash_timeout_work;

	enum ktd2692_led_mode mode;
	bool flash_strobe_active;
};

static struct ktd2692_context *fled_cdev_to_led(
				struct led_classdev_flash *fled_cdev)
{
	return container_of(fled_cdev, struct ktd2692_context, fled_cdev);
}

static void ktd2692_set_mode(struct ktd2692_context *led,
			     enum ktd2692_led_mode mode)
{
	expresswire_write_u8(&led->props, mode | KTD2692_REG_MODE_BASE);
	led->mode = mode;
}

static void ktd2692_disable_locked(struct ktd2692_context *led)
{
	gpiod_set_value_cansleep(led->aux_gpio, 0);
	ktd2692_set_mode(led, KTD2692_MODE_DISABLE);
	led->flash_strobe_active = false;
}

static void ktd2692_flash_timeout_work(struct work_struct *work)
{
	struct ktd2692_context *led =
		container_of(to_delayed_work(work), struct ktd2692_context,
			     flash_timeout_work);

	mutex_lock(&led->lock);
	ktd2692_disable_locked(led);
	mutex_unlock(&led->lock);
}

static void ktd2692_set_flash_brightness(struct ktd2692_context *led,
					 u32 brightness)
{
	struct led_flash_setting *setting = &led->fled_cdev.brightness;
	u32 level;

	level = DIV_ROUND_CLOSEST(brightness, setting->step);
	level = clamp(level, 1U, KTD2692_FLASH_MODE_CURRENT_LEVELS) - 1;
	expresswire_write_u8(&led->props, level |
					KTD2692_REG_FLASH_CURRENT_BASE);
}

static int ktd2692_led_brightness_set(struct led_classdev *led_cdev,
				       enum led_brightness brightness)
{
	struct led_classdev_flash *fled_cdev = lcdev_to_flcdev(led_cdev);
	struct ktd2692_context *led = fled_cdev_to_led(fled_cdev);

	cancel_delayed_work_sync(&led->flash_timeout_work);
	mutex_lock(&led->lock);
	gpiod_set_value_cansleep(led->aux_gpio, 0);
	led->flash_strobe_active = false;

	if (brightness == LED_OFF) {
		ktd2692_disable_locked(led);
	} else {
		expresswire_write_u8(&led->props, brightness |
					KTD2692_REG_MOVIE_CURRENT_BASE);
		ktd2692_set_mode(led, KTD2692_MODE_MOVIE);
	}

	mutex_unlock(&led->lock);

	return 0;
}

static int ktd2692_led_flash_brightness_set(struct led_classdev_flash *fled_cdev,
					    u32 brightness)
{
	struct ktd2692_context *led = fled_cdev_to_led(fled_cdev);

	mutex_lock(&led->lock);
	ktd2692_set_flash_brightness(led, brightness);
	mutex_unlock(&led->lock);

	return 0;
}

static int ktd2692_led_flash_strobe_set(struct led_classdev_flash *fled_cdev,
					bool state)
{
	struct ktd2692_context *led = fled_cdev_to_led(fled_cdev);
	struct led_flash_setting *timeout = &fled_cdev->timeout;
	u32 flash_tm_reg, timeout_us = 0;

	cancel_delayed_work_sync(&led->flash_timeout_work);
	mutex_lock(&led->lock);

	if (state) {
		flash_tm_reg = GET_TIMEOUT_OFFSET(timeout->val, timeout->step);
		timeout_us = ktd2692_flash_timeout_us[flash_tm_reg];
		expresswire_write_u8(&led->props, flash_tm_reg
				| KTD2692_REG_FLASH_TIMEOUT_BASE);

		ktd2692_set_mode(led, KTD2692_MODE_FLASH);
		gpiod_set_value_cansleep(led->aux_gpio, 1);
		led->flash_strobe_active = true;
	} else {
		ktd2692_disable_locked(led);
	}

	fled_cdev->led_cdev.brightness = LED_OFF;
	if (state && timeout_us)
		mod_delayed_work(system_wq, &led->flash_timeout_work,
				 usecs_to_jiffies(timeout_us));

	mutex_unlock(&led->lock);

	return 0;
}

static int ktd2692_led_flash_strobe_get(struct led_classdev_flash *fled_cdev,
					bool *state)
{
	struct ktd2692_context *led = fled_cdev_to_led(fled_cdev);

	mutex_lock(&led->lock);
	*state = led->flash_strobe_active;
	mutex_unlock(&led->lock);

	return 0;
}

static int ktd2692_led_flash_timeout_set(struct led_classdev_flash *fled_cdev,
					 u32 timeout)
{
	return 0;
}

static void ktd2692_init_movie_current_max(struct ktd2692_led_config_data *cfg)
{
	u32 offset, step;
	u32 movie_current_microamp;

	offset = KTD2692_MOVIE_MODE_CURRENT_LEVELS;
	step = KTD2692_MM_TO_FL_RATIO(cfg->flash_max_microamp)
		/ KTD2692_MOVIE_MODE_CURRENT_LEVELS;

	do {
		movie_current_microamp = step * offset;
		offset--;
	} while ((movie_current_microamp > cfg->movie_max_microamp) &&
		(offset > 0));

	cfg->max_brightness = offset;
}

static void ktd2692_init_flash_timeout(struct led_classdev_flash *fled_cdev,
				       struct ktd2692_led_config_data *cfg)
{
	struct led_flash_setting *setting;
	u32 level;

	setting = &fled_cdev->timeout;
	setting->min = KTD2692_FLASH_MODE_TIMEOUT_DISABLE;
	setting->step = KTD2692_FLASH_MODE_TIMEOUT_STEP_US;
	for (level = 1; level < ARRAY_SIZE(ktd2692_flash_timeout_us); level++)
		if (ktd2692_flash_timeout_us[level] > cfg->flash_max_timeout)
			break;

	setting->max = setting->step * (level - 1);
	setting->val = setting->max;
}

static void ktd2692_init_flash_brightness(struct led_classdev_flash *fled_cdev,
					  struct ktd2692_led_config_data *cfg)
{
	struct led_flash_setting *setting = &fled_cdev->brightness;

	setting->step = max(cfg->flash_max_microamp /
			    KTD2692_FLASH_MODE_CURRENT_LEVELS, 1U);
	setting->min = setting->step;
	setting->max = setting->step * KTD2692_FLASH_MODE_CURRENT_LEVELS;
	setting->val = setting->step * KTD2692_FLASH_MODE_DEFAULT_LEVEL;
}

static void ktd2692_setup(struct ktd2692_context *led)
{
	led->mode = KTD2692_MODE_DISABLE;
	led->flash_strobe_active = false;
	gpiod_set_value_cansleep(led->aux_gpio, 0);
	expresswire_power_off(&led->props);

	/* Select the lowest threshold so the timeout covers the widest range. */
	expresswire_write_u8(&led->props,
			     KTD2692_REG_MM_MIN_CURR_THRESHOLD_BASE);
	ktd2692_set_flash_brightness(led, led->fled_cdev.brightness.val);
}

static void regulator_disable_action(void *_data)
{
	struct ktd2692_context *led = _data;
	int ret;

	ret = regulator_disable(led->regulator);
	if (ret)
		dev_err(led->dev, "Failed to disable supply: %d\n", ret);
}

static int ktd2692_parse_dt(struct ktd2692_context *led, struct device *dev,
			    struct device_node *child_node,
			    struct ktd2692_led_config_data *cfg)
{
	struct device_node *np = dev_of_node(dev);
	int ret;

	if (!np)
		return -ENXIO;

	led->props.ctrl_gpio = devm_gpiod_get(dev, "ctrl", GPIOD_OUT_HIGH);
	ret = PTR_ERR_OR_ZERO(led->props.ctrl_gpio);
	if (ret)
		return dev_err_probe(dev, ret, "cannot get ctrl-gpios\n");

	led->aux_gpio = devm_gpiod_get_optional(dev, "aux", GPIOD_OUT_LOW);
	if (IS_ERR(led->aux_gpio))
		return dev_err_probe(dev, PTR_ERR(led->aux_gpio), "cannot get aux-gpios\n");

	led->regulator = devm_regulator_get_optional(dev, "vin");
	if (IS_ERR(led->regulator)) {
		ret = PTR_ERR(led->regulator);
		if (ret != -ENODEV)
			return dev_err_probe(dev, ret, "cannot get vin supply\n");

		led->regulator = NULL;
	}

	if (led->regulator) {
		ret = regulator_enable(led->regulator);
		if (ret)
			return dev_err_probe(dev, ret, "failed to enable vin supply\n");

		ret = devm_add_action_or_reset(dev, regulator_disable_action, led);
		if (ret)
			return ret;
	}

	ret = of_property_read_u32(child_node, "led-max-microamp",
				   &cfg->movie_max_microamp);
	if (ret) {
		dev_err(dev, "failed to parse led-max-microamp\n");
		return ret;
	}

	ret = of_property_read_u32(child_node, "flash-max-microamp",
				   &cfg->flash_max_microamp);
	if (ret) {
		dev_err(dev, "failed to parse flash-max-microamp\n");
		return ret;
	}

	ret = of_property_read_u32(child_node, "flash-max-timeout-us",
				   &cfg->flash_max_timeout);
	if (ret) {
		dev_err(dev, "failed to parse flash-max-timeout-us\n");
		return ret;
	}

	if (cfg->flash_max_microamp < KTD2692_MOVIE_MODE_CURRENT_LEVELS * 3 ||
	    !cfg->movie_max_microamp ||
	    cfg->movie_max_microamp >
		KTD2692_MM_TO_FL_RATIO(cfg->flash_max_microamp))
		return dev_err_probe(dev, -EINVAL,
				     "invalid movie/flash current limits\n");

	if (cfg->flash_max_timeout < ktd2692_flash_timeout_us[1] ||
	    cfg->flash_max_timeout > KTD2692_FLASH_MODE_TIMEOUT_MAX_US)
		return dev_err_probe(dev, -EINVAL,
				     "invalid flash timeout limit\n");

	return 0;
}

static const struct led_flash_ops flash_ops = {
	.flash_brightness_set = ktd2692_led_flash_brightness_set,
	.strobe_set = ktd2692_led_flash_strobe_set,
	.strobe_get = ktd2692_led_flash_strobe_get,
	.timeout_set = ktd2692_led_flash_timeout_set,
};

static int ktd2692_probe(struct platform_device *pdev)
{
	struct device_node *child_node __free(device_node) = NULL;
	struct ktd2692_context *led;
	struct led_classdev *led_cdev;
	struct led_classdev_flash *fled_cdev;
	struct ktd2692_led_config_data led_cfg;
	struct led_init_data init_data = {};
	int ret;

	led = devm_kzalloc(&pdev->dev, sizeof(*led), GFP_KERNEL);
	if (!led)
		return -ENOMEM;
	led->dev = &pdev->dev;
	platform_set_drvdata(pdev, led);

	fled_cdev = &led->fled_cdev;
	led_cdev = &fled_cdev->led_cdev;
	led->props.timing = ktd2692_timing;

	child_node = of_get_next_available_child(pdev->dev.of_node, NULL);
	if (!child_node)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "no child node found for connected LED\n");

	init_data.fwnode = of_fwnode_handle(child_node);

	ret = ktd2692_parse_dt(led, &pdev->dev, child_node, &led_cfg);
	if (ret)
		return ret;

	ktd2692_init_flash_timeout(fled_cdev, &led_cfg);
	ktd2692_init_flash_brightness(fled_cdev, &led_cfg);
	ktd2692_init_movie_current_max(&led_cfg);

	fled_cdev->ops = &flash_ops;

	led_cdev->max_brightness = led_cfg.max_brightness;
	led_cdev->brightness_set_blocking = ktd2692_led_brightness_set;
	led_cdev->flags |= LED_CORE_SUSPENDRESUME | LED_DEV_CAP_FLASH;

	mutex_init(&led->lock);
	INIT_DELAYED_WORK(&led->flash_timeout_work, ktd2692_flash_timeout_work);
	ktd2692_setup(led);

	ret = led_classdev_flash_register_ext(&pdev->dev, fled_cdev, &init_data);
	if (ret) {
		dev_err(&pdev->dev, "can't register flash LED: %d\n", ret);
		expresswire_power_off(&led->props);
		mutex_destroy(&led->lock);
		return ret;
	}

	return 0;
}

static void ktd2692_remove(struct platform_device *pdev)
{
	struct ktd2692_context *led = platform_get_drvdata(pdev);

	led_classdev_flash_unregister(&led->fled_cdev);
	ktd2692_led_brightness_set(&led->fled_cdev.led_cdev, LED_OFF);
	expresswire_power_off(&led->props);

	mutex_destroy(&led->lock);
}

static void ktd2692_shutdown(struct platform_device *pdev)
{
	struct ktd2692_context *led = platform_get_drvdata(pdev);

	ktd2692_led_brightness_set(&led->fled_cdev.led_cdev, LED_OFF);
	expresswire_power_off(&led->props);
}

static const struct of_device_id ktd2692_match[] = {
	{ .compatible = "kinetic,ktd2692", },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, ktd2692_match);

static struct platform_driver ktd2692_driver = {
	.driver = {
		.name  = "ktd2692",
		.of_match_table = ktd2692_match,
	},
	.probe  = ktd2692_probe,
	.remove = ktd2692_remove,
	.shutdown = ktd2692_shutdown,
};

module_platform_driver(ktd2692_driver);

MODULE_IMPORT_NS("EXPRESSWIRE");
MODULE_AUTHOR("Ingi Kim <ingi2.kim@samsung.com>");
MODULE_DESCRIPTION("Kinetic KTD2692 LED driver");
MODULE_LICENSE("GPL v2");
