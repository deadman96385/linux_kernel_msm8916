// SPDX-License-Identifier: GPL-2.0-only
/*
 * Maxim MAX77849 companion PMIC core
 *
 * The MAX77849 has separate I2C addresses for the PMIC/charger and MUIC.
 * Charger and OTG mode share one register, so the core serializes mode changes
 * requested by the charger and MUIC children.
 */

#include <linux/i2c.h>
#include <linux/mfd/core.h>
#include <linux/mfd/max77849.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/slab.h>

static const struct mfd_cell max77849_cells[] = {
	{
		.name = "max77849-charger",
		.of_compatible = "maxim,max77849-charger",
	},
	{
		.name = "max77849-muic",
		.of_compatible = "maxim,max77849-muic",
	},
};

static const struct regmap_config max77849_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = MAX77849_REG_SAFEOUT_CTRL,
};

static const struct regmap_config max77849_muic_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = MAX77849_MUIC_REG_CTRL4,
};

static int max77849_update_mode_locked(struct max77849 *max77849)
{
	unsigned int desired;
	unsigned int protection;
	int ret;

	if (max77849->otg_enabled)
		desired = MAX77849_CHG_CNFG_00_OTG |
			  MAX77849_CHG_CNFG_00_BOOST |
			  MAX77849_CHG_CNFG_00_DIS_MUIC_CTRL;
	else
		desired = MAX77849_CHG_CNFG_00_BUCK |
			  (max77849->charge_requested ?
			   MAX77849_CHG_CNFG_00_CHG : 0);

	ret = regmap_update_bits(max77849->regmap, MAX77849_REG_CHG_CNFG_06,
				 MAX77849_CHG_CNFG_06_CHGPROT_MASK,
				 MAX77849_CHG_CNFG_06_CHGPROT_UNLOCK);
	if (ret)
		return ret;
	ret = regmap_read(max77849->regmap, MAX77849_REG_CHG_CNFG_06,
			  &protection);
	if (ret)
		return ret;
	if ((protection & MAX77849_CHG_CNFG_06_CHGPROT_MASK) !=
	    MAX77849_CHG_CNFG_06_CHGPROT_UNLOCK)
		return -EACCES;

	if (max77849->otg_enabled) {
		ret = regmap_write(max77849->regmap, MAX77849_REG_CHG_CNFG_11,
				   MAX77849_CHG_CNFG_11_OTG_5V);
		if (ret)
			return ret;
	}

	ret = regmap_update_bits(max77849->regmap,
				 MAX77849_REG_CHG_CNFG_00,
				 MAX77849_CHG_CNFG_00_MODE_MASK, desired);
	if (ret) {
		if (max77849->otg_enabled)
			regmap_write(max77849->regmap,
				     MAX77849_REG_CHG_CNFG_11,
				     MAX77849_CHG_CNFG_11_OTG_OFF);
		return ret;
	}

	if (!max77849->otg_enabled &&
	    regmap_write(max77849->regmap, MAX77849_REG_CHG_CNFG_11,
			 MAX77849_CHG_CNFG_11_OTG_OFF))
		dev_warn(max77849->dev,
			 "failed to reset disabled OTG voltage setting\n");

	return 0;
}

int max77849_set_charging(struct max77849 *max77849, bool enable)
{
	bool old_requested;
	int ret;

	mutex_lock(&max77849->mode_lock);
	old_requested = max77849->charge_requested;
	max77849->charge_requested = enable;
	ret = max77849_update_mode_locked(max77849);
	if (ret)
		max77849->charge_requested = old_requested;
	mutex_unlock(&max77849->mode_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(max77849_set_charging);

int max77849_set_otg(struct max77849 *max77849, bool enable)
{
	bool old_enabled;
	int ret;

	mutex_lock(&max77849->mode_lock);
	old_enabled = max77849->otg_enabled;
	max77849->otg_enabled = enable;
	ret = max77849_update_mode_locked(max77849);
	if (ret)
		max77849->otg_enabled = old_enabled;
	mutex_unlock(&max77849->mode_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(max77849_set_otg);

bool max77849_charging_active(struct max77849 *max77849)
{
	bool active;

	mutex_lock(&max77849->mode_lock);
	active = max77849->charge_requested && !max77849->otg_enabled;
	mutex_unlock(&max77849->mode_lock);

	return active;
}
EXPORT_SYMBOL_GPL(max77849_charging_active);

static int max77849_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct i2c_client *muic;
	struct max77849 *max77849;
	unsigned int id1, id2;
	int ret;

	max77849 = devm_kzalloc(dev, sizeof(*max77849), GFP_KERNEL);
	if (!max77849)
		return -ENOMEM;

	max77849->dev = dev;
	max77849->irq = client->irq;
	mutex_init(&max77849->mode_lock);
	i2c_set_clientdata(client, max77849);

	max77849->regmap = devm_regmap_init_i2c(client,
						&max77849_regmap_config);
	if (IS_ERR(max77849->regmap))
		return dev_err_probe(dev, PTR_ERR(max77849->regmap),
				     "failed to initialize regmap\n");

	ret = regmap_read(max77849->regmap, MAX77849_REG_PMIC_ID1, &id1);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read PMIC ID1\n");

	ret = regmap_read(max77849->regmap, MAX77849_REG_PMIC_ID2, &id2);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read PMIC ID2\n");

	dev_info(dev, "MAX77849 detected (ID1=0x%02x, ID2=0x%02x)\n",
		 id1, id2);

	muic = devm_i2c_new_dummy_device(dev, client->adapter,
					 MAX77849_I2C_ADDR_MUIC);
	if (IS_ERR(muic))
		return dev_err_probe(dev, PTR_ERR(muic),
				     "failed to register MUIC I2C client\n");

	max77849->regmap_muic = devm_regmap_init_i2c(muic,
						     &max77849_muic_regmap_config);
	if (IS_ERR(max77849->regmap_muic))
		return dev_err_probe(dev, PTR_ERR(max77849->regmap_muic),
				     "failed to initialize MUIC regmap\n");

	return devm_mfd_add_devices(dev, PLATFORM_DEVID_NONE,
				    max77849_cells, ARRAY_SIZE(max77849_cells),
				    NULL, 0, NULL);
}

static const struct of_device_id max77849_of_match[] = {
	{ .compatible = "maxim,max77849" },
	{ }
};
MODULE_DEVICE_TABLE(of, max77849_of_match);

static struct i2c_driver max77849_driver = {
	.driver = {
		.name = "max77849",
		.of_match_table = max77849_of_match,
	},
	.probe = max77849_probe,
};
module_i2c_driver(max77849_driver);

MODULE_AUTHOR("Sean Hoyt <seanhoyt963@gmail.com>");
MODULE_DESCRIPTION("Maxim MAX77849 companion PMIC core driver");
MODULE_LICENSE("GPL");
