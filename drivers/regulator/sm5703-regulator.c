// SPDX-License-Identifier: GPL-2.0-only

#include <linux/mfd/sm5703.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/of_regulator.h>

enum sm5703_regulator_id {
	SM5703_BUCK,
	SM5703_LDO1,
	SM5703_LDO2,
	SM5703_LDO3,
	SM5703_USBLDO1,
	SM5703_USBLDO2,
	SM5703_VBUS,
	SM5703_MAX_REGULATORS,
};

static const unsigned int sm5703_ldo_voltages[] = {
	1500000, 1800000, 2600000, 2800000,
	3000000, 3300000, 3300000, 3300000,
};

static const unsigned int sm5703_buck_voltages[] = {
	1000000, 1000000, 1000000, 1000000,
	1000000, 1000000, 1000000, 1000000,
	1000000, 1000000, 1000000, 1100000,
	1200000, 1300000, 1400000, 1500000,
	1600000, 1700000, 1800000, 1900000,
	2000000, 2100000, 2200000, 2300000,
	2400000, 2500000, 2600000, 2700000,
	2800000, 2900000, 3000000, 3000000,
};

static const unsigned int sm5703_otg_currents[] = {
	500000, 700000, 900000, 1200000,
};

static const struct regulator_ops sm5703_regulator_ops = {
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.is_enabled = regulator_is_enabled_regmap,
	.list_voltage = regulator_list_voltage_table,
	.get_voltage_sel = regulator_get_voltage_sel_regmap,
	.set_voltage_sel = regulator_set_voltage_sel_regmap,
};

static const struct regulator_ops sm5703_fixed_regulator_ops = {
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.is_enabled = regulator_is_enabled_regmap,
};

static int sm5703_vbus_enable(struct regulator_dev *rdev)
{
	return sm5703_set_otg(rdev_get_drvdata(rdev), true);
}

static int sm5703_vbus_disable(struct regulator_dev *rdev)
{
	return sm5703_set_otg(rdev_get_drvdata(rdev), false);
}

static int sm5703_vbus_is_enabled(struct regulator_dev *rdev)
{
	return sm5703_otg_active(rdev_get_drvdata(rdev));
}

static const struct regulator_ops sm5703_vbus_ops = {
	.enable = sm5703_vbus_enable,
	.disable = sm5703_vbus_disable,
	.is_enabled = sm5703_vbus_is_enabled,
	.get_current_limit = regulator_get_current_limit_regmap,
	.set_current_limit = regulator_set_current_limit_regmap,
};

static const struct regulator_desc sm5703_regulators[] = {
	[SM5703_BUCK] = {
		.name = "buck",
		.of_match = "buck",
		.regulators_node = "regulators",
		.id = SM5703_BUCK,
		.type = REGULATOR_VOLTAGE,
		.owner = THIS_MODULE,
		.ops = &sm5703_regulator_ops,
		.volt_table = sm5703_buck_voltages,
		.n_voltages = ARRAY_SIZE(sm5703_buck_voltages),
		.vsel_reg = SM5703_REG_BUCK,
		.vsel_mask = SM5703_BUCK_VSEL_MASK,
		.enable_reg = SM5703_REG_BUCK,
		.enable_mask = SM5703_BUCK_ENABLE,
	},
	[SM5703_LDO1] = {
		.name = "ldo1",
		.of_match = "ldo1",
		.regulators_node = "regulators",
		.id = SM5703_LDO1,
		.type = REGULATOR_VOLTAGE,
		.owner = THIS_MODULE,
		.ops = &sm5703_regulator_ops,
		.volt_table = sm5703_ldo_voltages,
		.n_voltages = ARRAY_SIZE(sm5703_ldo_voltages),
		.vsel_reg = SM5703_REG_LDO1,
		.vsel_mask = SM5703_LDO_VSEL_MASK,
		.enable_reg = SM5703_REG_LDO1,
		.enable_mask = SM5703_LDO_ENABLE,
	},
	[SM5703_LDO2] = {
		.name = "ldo2",
		.of_match = "ldo2",
		.regulators_node = "regulators",
		.id = SM5703_LDO2,
		.type = REGULATOR_VOLTAGE,
		.owner = THIS_MODULE,
		.ops = &sm5703_regulator_ops,
		.volt_table = sm5703_ldo_voltages,
		.n_voltages = ARRAY_SIZE(sm5703_ldo_voltages),
		.vsel_reg = SM5703_REG_LDO2,
		.vsel_mask = SM5703_LDO_VSEL_MASK,
		.enable_reg = SM5703_REG_LDO2,
		.enable_mask = SM5703_LDO_ENABLE,
	},
	[SM5703_LDO3] = {
		.name = "ldo3",
		.of_match = "ldo3",
		.regulators_node = "regulators",
		.id = SM5703_LDO3,
		.type = REGULATOR_VOLTAGE,
		.owner = THIS_MODULE,
		.ops = &sm5703_regulator_ops,
		.volt_table = sm5703_ldo_voltages,
		.n_voltages = ARRAY_SIZE(sm5703_ldo_voltages),
		.vsel_reg = SM5703_REG_LDO3,
		.vsel_mask = SM5703_LDO_VSEL_MASK,
		.enable_reg = SM5703_REG_LDO3,
		.enable_mask = SM5703_LDO_ENABLE,
	},
	[SM5703_USBLDO1] = {
		.name = "usbldo1",
		.of_match = "usbldo1",
		.regulators_node = "regulators",
		.id = SM5703_USBLDO1,
		.type = REGULATOR_VOLTAGE,
		.owner = THIS_MODULE,
		.ops = &sm5703_fixed_regulator_ops,
		.n_voltages = 1,
		.fixed_uV = 4800000,
		.enable_reg = SM5703_REG_CNTL,
		.enable_mask = SM5703_CNTL_USBLDO1_EN,
	},
	[SM5703_USBLDO2] = {
		.name = "usbldo2",
		.of_match = "usbldo2",
		.regulators_node = "regulators",
		.id = SM5703_USBLDO2,
		.type = REGULATOR_VOLTAGE,
		.owner = THIS_MODULE,
		.ops = &sm5703_fixed_regulator_ops,
		.n_voltages = 1,
		.fixed_uV = 4800000,
		.enable_reg = SM5703_REG_CNTL,
		.enable_mask = SM5703_CNTL_USBLDO2_EN,
	},
	[SM5703_VBUS] = {
		.name = "vbus",
		.of_match = "vbus",
		.regulators_node = "regulators",
		.id = SM5703_VBUS,
		.type = REGULATOR_VOLTAGE,
		.owner = THIS_MODULE,
		.ops = &sm5703_vbus_ops,
		.n_voltages = 1,
		.fixed_uV = 5000000,
		.curr_table = sm5703_otg_currents,
		.n_current_limits = ARRAY_SIZE(sm5703_otg_currents),
		.csel_reg = SM5703_REG_OTGCURRENTCNTL,
		.csel_mask = SM5703_OTGCURRENTCNTL_MASK,
	},
};

static irqreturn_t sm5703_vbus_fault_irq(int irq, void *data)
{
	struct regulator_dev *rdev = data;
	struct sm5703 *sm5703 = rdev_get_drvdata(rdev);
	int ret;

	/* Downstream identifies OTGFAIL as the VBUS boost over-current fault. */
	ret = sm5703_set_otg(sm5703, false);
	if (ret)
		dev_err_ratelimited(sm5703->dev,
				    "failed to disable VBUS after over-current: %d\n",
				    ret);

	regulator_notifier_call_chain(rdev, REGULATOR_EVENT_OVER_CURRENT, NULL);

	return IRQ_HANDLED;
}

static int sm5703_regulator_probe(struct platform_device *pdev)
{
	struct sm5703 *sm5703 = dev_get_drvdata(pdev->dev.parent);
	struct regulator_config config = { };
	struct regulator_dev *rdev, *vbus_rdev = NULL;
	int irq, ret, i;

	config.dev = pdev->dev.parent;
	config.driver_data = sm5703;
	config.regmap = sm5703->regmap;

	for (i = 0; i < SM5703_MAX_REGULATORS; i++) {
		rdev = devm_regulator_register(&pdev->dev, &sm5703_regulators[i],
					       &config);
		if (IS_ERR(rdev))
			return dev_err_probe(&pdev->dev, PTR_ERR(rdev),
					     "failed to register %s\n",
					     sm5703_regulators[i].name);
		if (i == SM5703_VBUS)
			vbus_rdev = rdev;
	}

	irq = platform_get_irq_byname(pdev, "otg-fail");
	if (irq < 0)
		return irq;

	ret = devm_request_threaded_irq(&pdev->dev, irq, NULL,
					sm5703_vbus_fault_irq, IRQF_ONESHOT,
					"sm5703-vbus-over-current", vbus_rdev);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to request VBUS fault interrupt\n");

	return 0;
}

static const struct platform_device_id sm5703_regulator_id[] = {
	{ "sm5703-regulator" },
	{ }
};
MODULE_DEVICE_TABLE(platform, sm5703_regulator_id);

static struct platform_driver sm5703_regulator_driver = {
	.driver = {
		.name = "sm5703-regulator",
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
	.probe = sm5703_regulator_probe,
	.id_table = sm5703_regulator_id,
};
module_platform_driver(sm5703_regulator_driver);

MODULE_AUTHOR("Sean Hoyt <seanhoyt963@gmail.com>");
MODULE_DESCRIPTION("Silicon Mitus SM5703 regulator driver");
MODULE_LICENSE("GPL");
