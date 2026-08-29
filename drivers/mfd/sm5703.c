// SPDX-License-Identifier: GPL-2.0-only
/*
 * Silicon Mitus SM5703 multi-function device core
 *
 * The charger and OTG boost converter share the operation-mode field in
 * SM5703_REG_CNTL. Keep ownership of that field in the core so regulator,
 * charger and future flash-LED drivers cannot clobber each other's state.
 */

#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/mfd/core.h>
#include <linux/mfd/sm5703.h>
#include <linux/module.h>
#include <linux/regmap.h>

#define SM5703_REGMAP_IRQ(_offset, _bit) \
	{ .reg_offset = (_offset), .mask = BIT(_bit) }

static const struct regmap_irq sm5703_irqs[SM5703_IRQ_MAX] = {
	[SM5703_IRQ_AICL] = SM5703_REGMAP_IRQ(0, 0),
	[SM5703_IRQ_BATOVP] = SM5703_REGMAP_IRQ(0, 1),
	[SM5703_IRQ_LOWBAT] = SM5703_REGMAP_IRQ(0, 2),
	[SM5703_IRQ_VBUSLIMIT] = SM5703_REGMAP_IRQ(0, 3),
	[SM5703_IRQ_DISLIMIT] = SM5703_REGMAP_IRQ(0, 4),
	[SM5703_IRQ_VSYSOLP] = SM5703_REGMAP_IRQ(0, 5),
	[SM5703_IRQ_OTGFAIL] = SM5703_REGMAP_IRQ(0, 6),
	[SM5703_IRQ_THERMREG] = SM5703_REGMAP_IRQ(1, 0),
	[SM5703_IRQ_THERMSHDN] = SM5703_REGMAP_IRQ(1, 1),
	[SM5703_IRQ_VSYSNG] = SM5703_REGMAP_IRQ(1, 2),
	[SM5703_IRQ_VSYSOK] = SM5703_REGMAP_IRQ(1, 3),
	[SM5703_IRQ_NOBAT] = SM5703_REGMAP_IRQ(1, 4),
	[SM5703_IRQ_PRETMROFF] = SM5703_REGMAP_IRQ(1, 5),
	[SM5703_IRQ_FASTTMROFF] = SM5703_REGMAP_IRQ(1, 6),
	[SM5703_IRQ_CHGON] = SM5703_REGMAP_IRQ(2, 0),
	[SM5703_IRQ_Q4FULLON] = SM5703_REGMAP_IRQ(2, 1),
	[SM5703_IRQ_TOPOFF] = SM5703_REGMAP_IRQ(2, 2),
	[SM5703_IRQ_DONE] = SM5703_REGMAP_IRQ(2, 3),
	[SM5703_IRQ_CHGRSTF] = SM5703_REGMAP_IRQ(2, 4),
	[SM5703_IRQ_FLEDSHORT] = SM5703_REGMAP_IRQ(3, 0),
	[SM5703_IRQ_FLEDOPEN] = SM5703_REGMAP_IRQ(3, 1),
	[SM5703_IRQ_BOOSTPOK_NG] = SM5703_REGMAP_IRQ(3, 2),
	[SM5703_IRQ_BOOSTPOK] = SM5703_REGMAP_IRQ(3, 3),
	[SM5703_IRQ_ABSTMRON] = SM5703_REGMAP_IRQ(3, 4),
};

static const struct regmap_irq_chip sm5703_irq_chip = {
	.name = "sm5703",
	.status_base = SM5703_REG_INT1,
	.mask_base = SM5703_REG_INTMSK1,
	.num_regs = 4,
	.irqs = sm5703_irqs,
	.num_irqs = ARRAY_SIZE(sm5703_irqs),
	.init_ack_masked = true,
};

static const struct resource sm5703_charger_resources[] = {
	DEFINE_RES_IRQ_NAMED(SM5703_IRQ_AICL, "aicl"),
	DEFINE_RES_IRQ_NAMED(SM5703_IRQ_NOBAT, "no-battery"),
	DEFINE_RES_IRQ_NAMED(SM5703_IRQ_CHGON, "charging"),
	DEFINE_RES_IRQ_NAMED(SM5703_IRQ_TOPOFF, "top-off"),
	DEFINE_RES_IRQ_NAMED(SM5703_IRQ_DONE, "done"),
};

static const struct resource sm5703_regulator_resources[] = {
	DEFINE_RES_IRQ_NAMED(SM5703_IRQ_OTGFAIL, "otg-fail"),
};

static const struct mfd_cell sm5703_cells[] = {
	{
		.name = "sm5703-regulator",
		.resources = sm5703_regulator_resources,
		.num_resources = ARRAY_SIZE(sm5703_regulator_resources),
	}, {
		.name = "sm5703-charger",
		.of_compatible = "siliconmitus,sm5703-charger",
		.resources = sm5703_charger_resources,
		.num_resources = ARRAY_SIZE(sm5703_charger_resources),
	},
};

static const struct regmap_config sm5703_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = SM5703_REG_STATUS5,
};

static int sm5703_update_mode_locked(struct sm5703 *sm5703)
{
	unsigned int mode;
	int ret;

	if (sm5703->otg_enabled) {
		ret = regmap_update_bits(sm5703->regmap, SM5703_REG_FLEDCNTL6,
					 SM5703_FLEDCNTL6_BSTOUT_MASK,
					 SM5703_FLEDCNTL6_BSTOUT_5P0V);
		if (ret)
			return ret;

		mode = SM5703_CNTL_USB_OTG;
	} else {
		mode = sm5703->charge_requested ? SM5703_CNTL_CHARGING_ON :
						   SM5703_CNTL_CHARGING_OFF;
	}

	ret = regmap_update_bits(sm5703->regmap, SM5703_REG_CNTL,
				 SM5703_CNTL_OPERATION_MODE_MASK, mode);
	if (ret)
		return ret;

	if (!sm5703->otg_enabled) {
		ret = regmap_update_bits(sm5703->regmap, SM5703_REG_FLEDCNTL6,
					 SM5703_FLEDCNTL6_BSTOUT_MASK,
					 SM5703_FLEDCNTL6_BSTOUT_4P5V);
		if (ret)
			dev_warn(sm5703->dev,
				 "failed to restore non-OTG boost voltage: %d\n", ret);
	}

	return 0;
}

int sm5703_set_charging(struct sm5703 *sm5703, bool enable)
{
	bool old_requested;
	int ret;

	mutex_lock(&sm5703->mode_lock);
	old_requested = sm5703->charge_requested;
	sm5703->charge_requested = enable;
	ret = sm5703_update_mode_locked(sm5703);
	if (ret)
		sm5703->charge_requested = old_requested;
	mutex_unlock(&sm5703->mode_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(sm5703_set_charging);

int sm5703_set_otg(struct sm5703 *sm5703, bool enable)
{
	unsigned int status;
	bool old_enabled;
	int ret;

	mutex_lock(&sm5703->mode_lock);
	if (enable && !sm5703->otg_enabled) {
		ret = regmap_read(sm5703->regmap, SM5703_REG_STATUS5,
				  &status);
		if (ret)
			goto out_unlock;

		if (status & (SM5703_STATUS5_VBUSOK |
			      SM5703_STATUS5_VBUSOVP)) {
			dev_warn_ratelimited(sm5703->dev,
					     "refusing OTG while external VBUS is present\n");
			ret = -EBUSY;
			goto out_unlock;
		}
	}

	old_enabled = sm5703->otg_enabled;
	sm5703->otg_enabled = enable;
	ret = sm5703_update_mode_locked(sm5703);
	if (ret)
		sm5703->otg_enabled = old_enabled;

out_unlock:
	mutex_unlock(&sm5703->mode_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(sm5703_set_otg);

bool sm5703_charging_active(struct sm5703 *sm5703)
{
	bool active;

	mutex_lock(&sm5703->mode_lock);
	active = sm5703->charge_requested && !sm5703->otg_enabled;
	mutex_unlock(&sm5703->mode_lock);

	return active;
}
EXPORT_SYMBOL_GPL(sm5703_charging_active);

bool sm5703_otg_active(struct sm5703 *sm5703)
{
	bool active;

	mutex_lock(&sm5703->mode_lock);
	active = sm5703->otg_enabled;
	mutex_unlock(&sm5703->mode_lock);

	return active;
}
EXPORT_SYMBOL_GPL(sm5703_otg_active);

static int sm5703_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct sm5703 *sm5703;
	unsigned int id;
	int ret;

	sm5703 = devm_kzalloc(dev, sizeof(*sm5703), GFP_KERNEL);
	if (!sm5703)
		return -ENOMEM;

	sm5703->dev = dev;
	mutex_init(&sm5703->mode_lock);
	i2c_set_clientdata(client, sm5703);

	sm5703->reset_gpio =
		devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(sm5703->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(sm5703->reset_gpio),
				     "failed to deassert reset\n");

	sm5703->regmap = devm_regmap_init_i2c(client, &sm5703_regmap_config);
	if (IS_ERR(sm5703->regmap))
		return dev_err_probe(dev, PTR_ERR(sm5703->regmap),
				     "failed to initialize regmap\n");

	ret = regmap_read(sm5703->regmap, SM5703_REG_DEVICE_ID, &id);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read device ID\n");

	dev_info(dev, "SM5703 detected (device ID 0x%02x)\n", id);

	if (client->irq <= 0)
		return dev_err_probe(dev, -EINVAL, "missing interrupt\n");

	ret = devm_regmap_add_irq_chip(dev, sm5703->regmap, client->irq,
				       IRQF_TRIGGER_FALLING | IRQF_ONESHOT, 0,
				       &sm5703_irq_chip, &sm5703->irq_data);
	if (ret)
		return dev_err_probe(dev, ret, "failed to add IRQ chip\n");

	/* The I2C parent and its MFD children do not perform DMA. */
	dev->coherent_dma_mask = 0;
	dev->dma_mask = &dev->coherent_dma_mask;

	ret = devm_mfd_add_devices(dev, PLATFORM_DEVID_AUTO, sm5703_cells,
				   ARRAY_SIZE(sm5703_cells), NULL, 0,
				   regmap_irq_get_domain(sm5703->irq_data));
	if (ret)
		return dev_err_probe(dev, ret, "failed to add child devices\n");

	return 0;
}

static const struct of_device_id sm5703_of_match[] = {
	{ .compatible = "siliconmitus,sm5703" },
	{ }
};
MODULE_DEVICE_TABLE(of, sm5703_of_match);

static const struct i2c_device_id sm5703_i2c_id[] = {
	{ "sm5703" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, sm5703_i2c_id);

static struct i2c_driver sm5703_driver = {
	.driver = {
		.name = "sm5703",
		.of_match_table = sm5703_of_match,
	},
	.probe = sm5703_probe,
	.id_table = sm5703_i2c_id,
};
module_i2c_driver(sm5703_driver);

MODULE_AUTHOR("Sean Hoyt <seanhoyt963@gmail.com>");
MODULE_DESCRIPTION("Silicon Mitus SM5703 multi-function device core");
MODULE_LICENSE("GPL");
