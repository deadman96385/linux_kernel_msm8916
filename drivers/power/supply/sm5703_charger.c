// SPDX-License-Identifier: GPL-2.0-only
/* Silicon Mitus SM5703 battery charger driver. */

#include <linux/devm-helpers.h>
#include <linux/extcon.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/limits.h>
#include <linux/mfd/sm5703.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>

#define SM5703_INPUT_CURRENT_MIN_UA	100000
#define SM5703_INPUT_CURRENT_MAX_UA	2100000
#define SM5703_INPUT_CURRENT_STEP_UA	50000
#define SM5703_FAST_CURRENT_MIN_UA	100000
#define SM5703_FAST_CURRENT_MAX_UA	2500000
#define SM5703_FAST_CURRENT_STEP_UA	50000
#define SM5703_FLOAT_VOLTAGE_MIN_UV	4120000
#define SM5703_FLOAT_VOLTAGE_MAX_UV	4430000
#define SM5703_FLOAT_VOLTAGE_STEP_UV	10000
#define SM5703_TERM_CURRENT_MIN_UA	100000
#define SM5703_TERM_CURRENT_MAX_UA	475000
#define SM5703_TERM_CURRENT_STEP_UA	25000
#define SM5703_AICL_VOLTAGE_MIN_UV	4300000
#define SM5703_AICL_VOLTAGE_MAX_UV	4900000
#define SM5703_AICL_VOLTAGE_STEP_UV	100000
#define SM5703_AICL_CURRENT_MIN_UA	300000
#define SM5703_AICL_CURRENT_STEP_UA	50000
#define SM5703_AICL_START_DELAY_MS	1200
#define SM5703_AICL_STEP_DELAY_MS	200
#define SM5703_MONITOR_INTERVAL_MS	10000

enum sm5703_thermal_state {
	SM5703_THERMAL_UNKNOWN,
	SM5703_THERMAL_NORMAL,
	SM5703_THERMAL_COLD,
	SM5703_THERMAL_HOT,
};

struct sm5703_charger {
	struct device *dev;
	struct sm5703 *sm5703;
	struct regmap *regmap;
	struct gpio_desc *enable_gpio;
	struct power_supply *psy;
	struct extcon_dev *extcon;
	struct notifier_block extcon_nb;
	struct work_struct extcon_work;
	struct delayed_work aicl_work;
	struct delayed_work monitor_work;
	/* Serializes source policy and charger register programming. */
	struct mutex lock;

	bool source_online;
	bool user_enabled;
	bool hw_charge_enabled;
	enum power_supply_usb_type usb_type;
	enum sm5703_thermal_state thermal_state;
	int input_current_ua;
	int fast_current_ua;
	int fast_current_max_ua;
	int float_voltage_uv;
	int term_current_ua;
	int dcp_input_current_ua;
	int aicl_voltage_uv;
	int temp_stop_min_decic;
	int temp_resume_min_decic;
	int temp_resume_max_decic;
	int temp_stop_max_decic;
	int monitor_interval_ms;
	int aicl_irq;
	bool aicl_irq_disabled;
};

static int sm5703_input_current_to_reg(int ua)
{
	ua = clamp(ua, SM5703_INPUT_CURRENT_MIN_UA,
		   SM5703_INPUT_CURRENT_MAX_UA);

	return (ua - SM5703_INPUT_CURRENT_MIN_UA) /
		SM5703_INPUT_CURRENT_STEP_UA;
}

static int sm5703_fast_current_to_reg(int ua)
{
	ua = clamp(ua, SM5703_FAST_CURRENT_MIN_UA,
		   SM5703_FAST_CURRENT_MAX_UA);

	return (ua - SM5703_FAST_CURRENT_MIN_UA) /
		SM5703_FAST_CURRENT_STEP_UA;
}

static int sm5703_float_voltage_to_reg(int uv)
{
	uv = clamp(uv, SM5703_FLOAT_VOLTAGE_MIN_UV,
		   SM5703_FLOAT_VOLTAGE_MAX_UV);

	return (uv - SM5703_FLOAT_VOLTAGE_MIN_UV) /
		SM5703_FLOAT_VOLTAGE_STEP_UV;
}

static int sm5703_term_current_to_reg(int ua)
{
	ua = clamp(ua, SM5703_TERM_CURRENT_MIN_UA,
		   SM5703_TERM_CURRENT_MAX_UA);

	return (ua - SM5703_TERM_CURRENT_MIN_UA) /
		SM5703_TERM_CURRENT_STEP_UA;
}

static int sm5703_set_input_current(struct sm5703_charger *charger, int ua)
{
	int reg = sm5703_input_current_to_reg(ua);
	int ret;

	ret = regmap_update_bits(charger->regmap, SM5703_REG_VBUSCNTL,
				 SM5703_VBUSCNTL_LIMIT_MASK, reg);
	if (!ret)
		charger->input_current_ua = SM5703_INPUT_CURRENT_MIN_UA +
			reg * SM5703_INPUT_CURRENT_STEP_UA;

	return ret;
}

static int sm5703_set_fast_current(struct sm5703_charger *charger, int ua)
{
	int reg = sm5703_fast_current_to_reg(ua);
	int ret;

	ret = regmap_update_bits(charger->regmap, SM5703_REG_CHGCNTL2,
				 SM5703_CHGCNTL2_FASTCHG_MASK, reg);
	if (!ret)
		charger->fast_current_ua = SM5703_FAST_CURRENT_MIN_UA +
			reg * SM5703_FAST_CURRENT_STEP_UA;

	return ret;
}

static int sm5703_set_float_voltage(struct sm5703_charger *charger, int uv)
{
	int reg = sm5703_float_voltage_to_reg(uv);
	int ret;

	ret = regmap_update_bits(charger->regmap, SM5703_REG_CHGCNTL3,
				 SM5703_CHGCNTL3_BATREG_MASK, reg);
	if (!ret)
		charger->float_voltage_uv = SM5703_FLOAT_VOLTAGE_MIN_UV +
			reg * SM5703_FLOAT_VOLTAGE_STEP_UV;

	return ret;
}

static int sm5703_set_term_current(struct sm5703_charger *charger, int ua)
{
	int reg = sm5703_term_current_to_reg(ua);
	int ret;

	ret = regmap_update_bits(charger->regmap, SM5703_REG_CHGCNTL4,
				 SM5703_CHGCNTL4_TOPOFF_MASK, reg << 3);
	if (!ret)
		charger->term_current_ua = SM5703_TERM_CURRENT_MIN_UA +
			reg * SM5703_TERM_CURRENT_STEP_UA;

	return ret;
}

static int sm5703_set_charging_locked(struct sm5703_charger *charger,
				      bool enable)
{
	int ret;

	if (!enable) {
		/* Drop the external enable first so an I2C error fails safe. */
		gpiod_set_value_cansleep(charger->enable_gpio, 0);
		ret = sm5703_set_charging(charger->sm5703, false);
		if (!ret || charger->enable_gpio)
			charger->hw_charge_enabled = false;
		return ret;
	}

	ret = sm5703_set_charging(charger->sm5703, true);
	if (ret)
		return ret;

	gpiod_set_value_cansleep(charger->enable_gpio, 1);
	charger->hw_charge_enabled =
		sm5703_charging_active(charger->sm5703);

	return 0;
}

static void sm5703_charger_remove(struct platform_device *pdev)
{
	struct sm5703_charger *charger = platform_get_drvdata(pdev);
	int ret;

	/* Stop new cable events before draining work which can enable charging. */
	devm_extcon_unregister_notifier_all(charger->dev, charger->extcon,
					    &charger->extcon_nb);
	cancel_work_sync(&charger->extcon_work);

	mutex_lock(&charger->lock);
	charger->source_online = false;
	charger->usb_type = POWER_SUPPLY_USB_TYPE_UNKNOWN;
	charger->user_enabled = false;
	ret = sm5703_set_charging_locked(charger, false);
	mutex_unlock(&charger->lock);
	if (ret)
		dev_err(charger->dev,
			"failed to stop charging during removal: %d\n", ret);

	/* source_online is now false, so the AICL IRQ cannot requeue work. */
	cancel_delayed_work_sync(&charger->aicl_work);
	cancel_delayed_work_sync(&charger->monitor_work);
}

static int sm5703_apply_charge_enable_locked(struct sm5703_charger *charger)
{
	bool enable = charger->source_online && charger->user_enabled &&
		charger->thermal_state == SM5703_THERMAL_NORMAL;

	return sm5703_set_charging_locked(charger, enable);
}

static int sm5703_get_status(struct sm5703_charger *charger, int *status)
{
	unsigned int val;
	int ret;

	if (!READ_ONCE(charger->source_online)) {
		*status = POWER_SUPPLY_STATUS_DISCHARGING;
		return 0;
	}
	if (!READ_ONCE(charger->user_enabled) ||
	    READ_ONCE(charger->thermal_state) != SM5703_THERMAL_NORMAL ||
	    !sm5703_charging_active(charger->sm5703)) {
		*status = POWER_SUPPLY_STATUS_NOT_CHARGING;
		return 0;
	}

	ret = regmap_read(charger->regmap, SM5703_REG_STATUS3, &val);
	if (ret)
		return ret;

	if (val & (SM5703_STATUS3_DONE | SM5703_STATUS3_TOPOFF))
		*status = POWER_SUPPLY_STATUS_FULL;
	else if (val & SM5703_STATUS3_CHGON)
		*status = POWER_SUPPLY_STATUS_CHARGING;
	else
		*status = POWER_SUPPLY_STATUS_NOT_CHARGING;

	return 0;
}

static int sm5703_get_health(struct sm5703_charger *charger, int *health)
{
	enum sm5703_thermal_state thermal_state;
	unsigned int status2, status5;
	int ret;

	ret = regmap_read(charger->regmap, SM5703_REG_STATUS2, &status2);
	if (ret)
		return ret;

	ret = regmap_read(charger->regmap, SM5703_REG_STATUS5, &status5);
	if (ret)
		return ret;
	thermal_state = READ_ONCE(charger->thermal_state);

	if (status2 & SM5703_STATUS2_NOBAT)
		*health = POWER_SUPPLY_HEALTH_NO_BATTERY;
	else if (status5 & SM5703_STATUS5_VBUSOVP)
		*health = POWER_SUPPLY_HEALTH_OVERVOLTAGE;
	else if (READ_ONCE(charger->source_online) &&
		 status5 & SM5703_STATUS5_VBUSUVLO)
		*health = POWER_SUPPLY_HEALTH_UNDERVOLTAGE;
	else if (thermal_state == SM5703_THERMAL_COLD)
		*health = POWER_SUPPLY_HEALTH_COLD;
	else if (thermal_state == SM5703_THERMAL_HOT)
		*health = POWER_SUPPLY_HEALTH_OVERHEAT;
	else if (thermal_state == SM5703_THERMAL_UNKNOWN)
		*health = POWER_SUPPLY_HEALTH_UNKNOWN;
	else
		*health = POWER_SUPPLY_HEALTH_GOOD;

	return 0;
}

static enum power_supply_property sm5703_charger_properties[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_CHARGE_TYPE,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_USB_TYPE,
	POWER_SUPPLY_PROP_CHARGE_BEHAVIOUR,
	POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
	POWER_SUPPLY_PROP_INPUT_VOLTAGE_LIMIT,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_CHARGE_TERM_CURRENT,
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_MANUFACTURER,
};

static int sm5703_charger_get_property(struct power_supply *psy,
				       enum power_supply_property psp,
				       union power_supply_propval *val)
{
	struct sm5703_charger *charger = power_supply_get_drvdata(psy);
	unsigned int reg;
	int ret = 0;

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		ret = sm5703_get_status(charger, &val->intval);
		break;
	case POWER_SUPPLY_PROP_CHARGE_TYPE:
		ret = sm5703_get_status(charger, &val->intval);
		if (!ret)
			val->intval = val->intval == POWER_SUPPLY_STATUS_CHARGING ?
				POWER_SUPPLY_CHARGE_TYPE_FAST :
				POWER_SUPPLY_CHARGE_TYPE_NONE;
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		ret = sm5703_get_health(charger, &val->intval);
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		ret = regmap_read(charger->regmap, SM5703_REG_STATUS2, &reg);
		if (!ret)
			val->intval = !(reg & SM5703_STATUS2_NOBAT);
		break;
	case POWER_SUPPLY_PROP_ONLINE:
		ret = regmap_read(charger->regmap, SM5703_REG_STATUS5, &reg);
		if (!ret)
			val->intval = READ_ONCE(charger->source_online) &&
				!!(reg & SM5703_STATUS5_VBUSOK);
		break;
	case POWER_SUPPLY_PROP_USB_TYPE:
		val->intval = READ_ONCE(charger->usb_type);
		break;
	case POWER_SUPPLY_PROP_CHARGE_BEHAVIOUR:
		val->intval = READ_ONCE(charger->user_enabled) ?
			POWER_SUPPLY_CHARGE_BEHAVIOUR_AUTO :
			POWER_SUPPLY_CHARGE_BEHAVIOUR_INHIBIT_CHARGE;
		break;
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		val->intval = READ_ONCE(charger->input_current_ua);
		break;
	case POWER_SUPPLY_PROP_INPUT_VOLTAGE_LIMIT:
		val->intval = charger->aicl_voltage_uv;
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
		val->intval = READ_ONCE(charger->fast_current_ua);
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX:
		val->intval = charger->fast_current_max_ua;
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE:
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE_MAX:
		val->intval = charger->float_voltage_uv;
		break;
	case POWER_SUPPLY_PROP_CHARGE_TERM_CURRENT:
		val->intval = charger->term_current_ua;
		break;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = "SM5703";
		break;
	case POWER_SUPPLY_PROP_MANUFACTURER:
		val->strval = "Silicon Mitus";
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int sm5703_charger_set_property(struct power_supply *psy,
				       enum power_supply_property psp,
				       const union power_supply_propval *val)
{
	struct sm5703_charger *charger = power_supply_get_drvdata(psy);
	bool old_enabled;
	int ret;

	mutex_lock(&charger->lock);
	switch (psp) {
	case POWER_SUPPLY_PROP_CHARGE_BEHAVIOUR:
		old_enabled = charger->user_enabled;
		if (val->intval == POWER_SUPPLY_CHARGE_BEHAVIOUR_AUTO) {
			charger->user_enabled = true;
		} else if (val->intval ==
			   POWER_SUPPLY_CHARGE_BEHAVIOUR_INHIBIT_CHARGE) {
			charger->user_enabled = false;
		} else {
			ret = -EINVAL;
			break;
		}

		ret = sm5703_apply_charge_enable_locked(charger);
		if (ret)
			charger->user_enabled = old_enabled;
		break;
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		if (val->intval < SM5703_INPUT_CURRENT_MIN_UA ||
		    val->intval > SM5703_INPUT_CURRENT_MAX_UA)
			ret = -EINVAL;
		else
			ret = sm5703_set_input_current(charger, val->intval);
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
		if (val->intval < SM5703_FAST_CURRENT_MIN_UA ||
		    val->intval > charger->fast_current_max_ua)
			ret = -EINVAL;
		else
			ret = sm5703_set_fast_current(charger, val->intval);
		break;
	default:
		ret = -EINVAL;
		break;
	}
	mutex_unlock(&charger->lock);

	if (!ret)
		power_supply_changed(charger->psy);

	return ret;
}

static int sm5703_charger_property_is_writeable(struct power_supply *psy,
						enum power_supply_property psp)
{
	return psp == POWER_SUPPLY_PROP_CHARGE_BEHAVIOUR ||
		psp == POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT ||
		psp == POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT;
}

static const struct power_supply_desc sm5703_charger_desc = {
	.name = "sm5703-charger",
	.type = POWER_SUPPLY_TYPE_USB,
	.usb_types = BIT(POWER_SUPPLY_USB_TYPE_UNKNOWN) |
		     BIT(POWER_SUPPLY_USB_TYPE_SDP) |
		     BIT(POWER_SUPPLY_USB_TYPE_CDP) |
		     BIT(POWER_SUPPLY_USB_TYPE_DCP),
	.properties = sm5703_charger_properties,
	.num_properties = ARRAY_SIZE(sm5703_charger_properties),
	.get_property = sm5703_charger_get_property,
	.set_property = sm5703_charger_set_property,
	.property_is_writeable = sm5703_charger_property_is_writeable,
	.charge_behaviours = BIT(POWER_SUPPLY_CHARGE_BEHAVIOUR_AUTO) |
			     BIT(POWER_SUPPLY_CHARGE_BEHAVIOUR_INHIBIT_CHARGE),
};

static int sm5703_apply_source_locked(struct sm5703_charger *charger,
				      enum power_supply_usb_type type)
{
	int input_ua, fast_ua;
	int ret;

	switch (type) {
	case POWER_SUPPLY_USB_TYPE_SDP:
		input_ua = 500000;
		fast_ua = 500000;
		break;
	case POWER_SUPPLY_USB_TYPE_CDP:
		input_ua = 1500000;
		fast_ua = 1500000;
		break;
	case POWER_SUPPLY_USB_TYPE_DCP:
		input_ua = charger->dcp_input_current_ua;
		fast_ua = charger->fast_current_max_ua;
		break;
	default:
		charger->source_online = false;
		charger->usb_type = POWER_SUPPLY_USB_TYPE_UNKNOWN;
		cancel_delayed_work(&charger->aicl_work);
		if (charger->aicl_irq_disabled) {
			charger->aicl_irq_disabled = false;
			enable_irq(charger->aicl_irq);
		}
		return sm5703_set_charging_locked(charger, false);
	}

	fast_ua = min(fast_ua, charger->fast_current_max_ua);
	ret = sm5703_set_input_current(charger, input_ua);
	if (ret)
		return ret;

	ret = sm5703_set_fast_current(charger, fast_ua);
	if (ret)
		return ret;

	charger->source_online = true;
	charger->usb_type = type;
	ret = sm5703_apply_charge_enable_locked(charger);
	if (ret) {
		charger->source_online = false;
		charger->usb_type = POWER_SUPPLY_USB_TYPE_UNKNOWN;
		return ret;
	}

	mod_delayed_work(system_wq, &charger->aicl_work,
			 msecs_to_jiffies(SM5703_AICL_START_DELAY_MS));
	return 0;
}

static void sm5703_aicl_work(struct work_struct *work)
{
	struct sm5703_charger *charger =
		container_of(to_delayed_work(work), struct sm5703_charger,
			     aicl_work);
	unsigned int status;
	int new_limit;
	int ret;

	mutex_lock(&charger->lock);
	if (!charger->source_online)
		goto out_unlock;

	ret = regmap_read(charger->regmap, SM5703_REG_STATUS1, &status);
	if (ret)
		goto out_warn;

	if (!(status & SM5703_STATUS1_AICL))
		goto out_enable_irq;

	if (charger->input_current_ua > SM5703_AICL_CURRENT_MIN_UA) {
		new_limit = charger->input_current_ua -
			SM5703_AICL_CURRENT_STEP_UA;
		ret = sm5703_set_input_current(charger, new_limit);
		if (ret)
			goto out_warn;

		mod_delayed_work(system_wq, &charger->aicl_work,
				 msecs_to_jiffies(SM5703_AICL_STEP_DELAY_MS));
		mutex_unlock(&charger->lock);
		power_supply_changed(charger->psy);
		return;
	}

	/* Keep a persistent at-minimum AICL condition masked until detach. */
	goto out_unlock;

out_enable_irq:
	if (charger->aicl_irq_disabled) {
		charger->aicl_irq_disabled = false;
		enable_irq(charger->aicl_irq);
	}
	mutex_unlock(&charger->lock);
	power_supply_changed(charger->psy);
	return;

out_warn:
	dev_warn_ratelimited(charger->dev, "AICL update failed: %d\n", ret);
	if (charger->source_online && charger->aicl_irq_disabled)
		mod_delayed_work(system_wq, &charger->aicl_work,
				 msecs_to_jiffies(SM5703_AICL_START_DELAY_MS));
out_unlock:
	mutex_unlock(&charger->lock);
}

static int sm5703_get_fuel_gauge_temp(struct sm5703_charger *charger,
				      int *temp_decic)
{
	union power_supply_propval val;
	struct power_supply *fuel_gauge;
	int ret;

	fuel_gauge = power_supply_get_by_reference(dev_fwnode(charger->dev),
						   "siliconmitus,monitored-fuel-gauge");
	if (IS_ERR(fuel_gauge))
		return PTR_ERR(fuel_gauge);
	if (!fuel_gauge)
		return -EPROBE_DEFER;

	ret = power_supply_get_property(fuel_gauge, POWER_SUPPLY_PROP_TEMP,
					&val);
	power_supply_put(fuel_gauge);
	if (ret)
		return ret;

	*temp_decic = val.intval;
	return 0;
}

static enum sm5703_thermal_state
sm5703_update_thermal_state(struct sm5703_charger *charger, int temp_decic)
{
	switch (charger->thermal_state) {
	case SM5703_THERMAL_COLD:
		if (temp_decic < charger->temp_resume_min_decic)
			return SM5703_THERMAL_COLD;
		break;
	case SM5703_THERMAL_HOT:
		if (temp_decic > charger->temp_resume_max_decic)
			return SM5703_THERMAL_HOT;
		break;
	default:
		break;
	}

	if (temp_decic <= charger->temp_stop_min_decic)
		return SM5703_THERMAL_COLD;
	if (temp_decic >= charger->temp_stop_max_decic)
		return SM5703_THERMAL_HOT;

	return SM5703_THERMAL_NORMAL;
}

static void sm5703_monitor_work(struct work_struct *work)
{
	struct sm5703_charger *charger =
		container_of(to_delayed_work(work), struct sm5703_charger,
			     monitor_work);
	enum sm5703_thermal_state old_thermal;
	bool old_enabled;
	bool changed = false;
	int temp_decic;
	int ret;

	ret = sm5703_get_fuel_gauge_temp(charger, &temp_decic);

	mutex_lock(&charger->lock);
	old_thermal = charger->thermal_state;
	old_enabled = charger->hw_charge_enabled;
	if (ret) {
		charger->thermal_state = SM5703_THERMAL_UNKNOWN;
		if (old_thermal == SM5703_THERMAL_UNKNOWN)
			dev_warn_ratelimited(charger->dev,
					     "charging stopped: fuel-gauge temperature unavailable (%d)\n",
					     ret);
	} else {
		charger->thermal_state =
			sm5703_update_thermal_state(charger, temp_decic);
	}

	if (old_thermal != charger->thermal_state) {
		changed = true;
		if (charger->thermal_state == SM5703_THERMAL_NORMAL)
			dev_info(charger->dev,
				 "battery temperature safe at %d.%d C\n",
				 temp_decic / 10, abs(temp_decic % 10));
		else if (charger->thermal_state == SM5703_THERMAL_COLD)
			dev_warn(charger->dev,
				 "charging stopped: battery cold at %d.%d C\n",
				 temp_decic / 10, abs(temp_decic % 10));
		else if (charger->thermal_state == SM5703_THERMAL_HOT)
			dev_warn(charger->dev,
				 "charging stopped: battery hot at %d.%d C\n",
				 temp_decic / 10, abs(temp_decic % 10));
		else
			dev_warn(charger->dev,
				 "charging stopped: fuel-gauge temperature unavailable (%d)\n",
				 ret);
	}

	ret = sm5703_apply_charge_enable_locked(charger);
	if (ret)
		dev_err_ratelimited(charger->dev,
				    "failed to update charger enable: %d\n",
				    ret);
	if (old_enabled != charger->hw_charge_enabled)
		changed = true;
	mutex_unlock(&charger->lock);

	if (changed)
		power_supply_changed(charger->psy);

	mod_delayed_work(system_wq, &charger->monitor_work,
			 msecs_to_jiffies(charger->monitor_interval_ms));
}

static void sm5703_extcon_work(struct work_struct *work)
{
	struct sm5703_charger *charger =
		container_of(work, struct sm5703_charger, extcon_work);
	enum power_supply_usb_type type = POWER_SUPPLY_USB_TYPE_UNKNOWN;
	int ret;

	if (extcon_get_state(charger->extcon, EXTCON_CHG_USB_DCP) > 0)
		type = POWER_SUPPLY_USB_TYPE_DCP;
	else if (extcon_get_state(charger->extcon, EXTCON_CHG_USB_CDP) > 0)
		type = POWER_SUPPLY_USB_TYPE_CDP;
	else if (extcon_get_state(charger->extcon, EXTCON_CHG_USB_SDP) > 0)
		type = POWER_SUPPLY_USB_TYPE_SDP;

	mutex_lock(&charger->lock);
	ret = sm5703_apply_source_locked(charger, type);
	mutex_unlock(&charger->lock);
	if (ret)
		dev_err(charger->dev, "failed to apply USB source: %d\n", ret);

	power_supply_changed(charger->psy);
}

static int sm5703_extcon_notifier(struct notifier_block *nb,
				  unsigned long event, void *ptr)
{
	struct sm5703_charger *charger =
		container_of(nb, struct sm5703_charger, extcon_nb);

	schedule_work(&charger->extcon_work);
	return NOTIFY_OK;
}

static irqreturn_t sm5703_charger_irq(int irq, void *data)
{
	struct sm5703_charger *charger = data;

	power_supply_changed(charger->psy);
	return IRQ_HANDLED;
}

static irqreturn_t sm5703_aicl_irq(int irq, void *data)
{
	struct sm5703_charger *charger = data;

	mutex_lock(&charger->lock);
	if (!charger->source_online)
		goto out_unlock;

	if (!charger->aicl_irq_disabled) {
		disable_irq_nosync(irq);
		charger->aicl_irq_disabled = true;
	}
	mod_delayed_work(system_wq, &charger->aicl_work,
			 msecs_to_jiffies(SM5703_AICL_START_DELAY_MS));

out_unlock:
	mutex_unlock(&charger->lock);
	return IRQ_HANDLED;
}

static int sm5703_request_irqs(struct platform_device *pdev,
			       struct sm5703_charger *charger)
{
	static const char * const status_irqs[] = {
		"no-battery", "charging", "top-off", "done",
	};
	int irq, ret, i;

	irq = platform_get_irq_byname(pdev, "aicl");
	if (irq < 0)
		return irq;
	charger->aicl_irq = irq;

	ret = devm_request_threaded_irq(charger->dev, irq, NULL,
					sm5703_aicl_irq, IRQF_ONESHOT,
					"sm5703-aicl", charger);
	if (ret)
		return ret;

	for (i = 0; i < ARRAY_SIZE(status_irqs); i++) {
		irq = platform_get_irq_byname(pdev, status_irqs[i]);
		if (irq < 0)
			return irq;

		ret = devm_request_threaded_irq(charger->dev, irq, NULL,
						sm5703_charger_irq, IRQF_ONESHOT,
						status_irqs[i], charger);
		if (ret)
			return ret;
	}

	return 0;
}

static int sm5703_hw_init(struct sm5703_charger *charger)
{
	unsigned int aicl, timer = 3;
	u32 timer_minutes = 45;
	int ret;

	ret = sm5703_set_charging(charger->sm5703, false);
	if (ret)
		return ret;

	ret = regmap_update_bits(charger->regmap, SM5703_REG_CNTL,
				 SM5703_CNTL_AUTOSET, SM5703_CNTL_AUTOSET);
	if (ret)
		return ret;

	ret = regmap_update_bits(charger->regmap, SM5703_REG_CHGCNTL4,
				 SM5703_CHGCNTL4_AUTOSTOP, 0);
	if (ret)
		return ret;

	aicl = (charger->aicl_voltage_uv - SM5703_AICL_VOLTAGE_MIN_UV) /
		SM5703_AICL_VOLTAGE_STEP_UV;
	ret = regmap_update_bits(charger->regmap, SM5703_REG_CHGCNTL5,
				 SM5703_CHGCNTL5_AICLTH_MASK |
				 SM5703_CHGCNTL5_AICLEN,
				 aicl | SM5703_CHGCNTL5_AICLEN);
	if (ret)
		return ret;

	device_property_read_u32(charger->dev,
				 "siliconmitus,topoff-timer-minutes",
				 &timer_minutes);
	switch (timer_minutes) {
	case 10:
		timer = 0;
		break;
	case 20:
		timer = 1;
		break;
	case 30:
		timer = 2;
		break;
	case 45:
		break;
	default:
		return dev_err_probe(charger->dev, -EINVAL,
				     "invalid top-off timer %u minutes\n",
				     timer_minutes);
	}

	ret = regmap_update_bits(charger->regmap, SM5703_REG_CHGCNTL5,
				 SM5703_CHGCNTL5_TOPOFF_TIMER_MASK,
				 timer << 5);
	if (ret)
		return ret;

	ret = regmap_update_bits(charger->regmap, SM5703_REG_CHGCNTL6,
				 SM5703_CHGCNTL6_FREQSEL_MASK,
				 SM5703_CHGCNTL6_FREQSEL_1P5MHZ);
	if (ret)
		return ret;

	ret = regmap_update_bits(charger->regmap, SM5703_REG_OTGCURRENTCNTL,
				 SM5703_OTGCURRENTCNTL_MASK,
				 SM5703_OTGCURRENTCNTL_1P2A);
	if (ret)
		return ret;

	if (device_property_read_bool(charger->dev,
				      "siliconmitus,boost-iq3-limit-1x")) {
		ret = regmap_update_bits(charger->regmap,
					 SM5703_REG_Q3LIMITCNTL,
					 SM5703_Q3LIMITCNTL_1X,
					 SM5703_Q3LIMITCNTL_1X);
		if (ret)
			return ret;
	}

	ret = sm5703_set_float_voltage(charger, charger->float_voltage_uv);
	if (ret)
		return ret;

	ret = sm5703_set_term_current(charger, charger->term_current_ua);
	if (ret)
		return ret;

	return sm5703_set_fast_current(charger,
				       charger->fast_current_max_ua);
}

static int sm5703_get_battery_info(struct sm5703_charger *charger)
{
	struct power_supply_battery_info *info;
	int ret;

	ret = power_supply_get_battery_info(charger->psy, &info);
	if (ret)
		return dev_err_probe(charger->dev, ret,
				     "failed to get battery information\n");

	charger->fast_current_max_ua = info->constant_charge_current_max_ua;
	charger->float_voltage_uv = info->constant_charge_voltage_max_uv;
	charger->term_current_ua = info->charge_term_current_ua;
	if (info->temp_min == INT_MIN || info->temp_alert_min == INT_MIN ||
	    info->temp_alert_max == INT_MAX || info->temp_max == INT_MAX) {
		power_supply_put_battery_info(charger->psy, info);
		return dev_err_probe(charger->dev, -EINVAL,
				     "battery temperature limits are required\n");
	}
	charger->temp_stop_min_decic = info->temp_min * 10;
	charger->temp_resume_min_decic = info->temp_alert_min * 10;
	charger->temp_resume_max_decic = info->temp_alert_max * 10;
	charger->temp_stop_max_decic = info->temp_max * 10;
	power_supply_put_battery_info(charger->psy, info);

	if (charger->fast_current_max_ua < SM5703_FAST_CURRENT_MIN_UA ||
	    charger->fast_current_max_ua > SM5703_FAST_CURRENT_MAX_UA ||
	    charger->float_voltage_uv < SM5703_FLOAT_VOLTAGE_MIN_UV ||
	    charger->float_voltage_uv > SM5703_FLOAT_VOLTAGE_MAX_UV ||
	    charger->term_current_ua < SM5703_TERM_CURRENT_MIN_UA ||
	    charger->term_current_ua > SM5703_TERM_CURRENT_MAX_UA)
		return dev_err_probe(charger->dev, -EINVAL,
				     "battery charging parameters out of range\n");
	if (charger->temp_stop_min_decic >=
			charger->temp_resume_min_decic ||
	    charger->temp_resume_min_decic >=
			charger->temp_resume_max_decic ||
	    charger->temp_resume_max_decic >=
			charger->temp_stop_max_decic)
		return dev_err_probe(charger->dev, -EINVAL,
				     "battery temperature limits are invalid\n");

	return 0;
}

static int sm5703_find_extcon(struct sm5703_charger *charger)
{
	struct device_node *connector, *parent;

	connector = of_parse_phandle(charger->dev->of_node,
				     "siliconmitus,usb-connector", 0);
	if (!connector)
		return dev_err_probe(charger->dev, -EINVAL,
				     "missing USB connector phandle\n");

	parent = of_get_parent(connector);
	of_node_put(connector);
	if (!parent)
		return -EINVAL;

	charger->extcon = extcon_find_edev_by_node(parent);
	of_node_put(parent);
	if (IS_ERR(charger->extcon))
		return dev_err_probe(charger->dev, PTR_ERR(charger->extcon),
				     "failed to find USB extcon device\n");

	return 0;
}

static int sm5703_charger_probe(struct platform_device *pdev)
{
	struct power_supply_config psy_config = { };
	struct sm5703_charger *charger;
	u32 value;
	int ret;

	charger = devm_kzalloc(&pdev->dev, sizeof(*charger), GFP_KERNEL);
	if (!charger)
		return -ENOMEM;

	charger->dev = &pdev->dev;
	charger->sm5703 = dev_get_drvdata(pdev->dev.parent);
	charger->regmap = charger->sm5703->regmap;
	charger->user_enabled = true;
	charger->usb_type = POWER_SUPPLY_USB_TYPE_UNKNOWN;
	charger->thermal_state = SM5703_THERMAL_UNKNOWN;
	charger->dcp_input_current_ua = 1000000;
	charger->aicl_voltage_uv = SM5703_AICL_VOLTAGE_MIN_UV;
	charger->monitor_interval_ms = SM5703_MONITOR_INTERVAL_MS;
	mutex_init(&charger->lock);
	platform_set_drvdata(pdev, charger);

	charger->enable_gpio = devm_gpiod_get_optional(charger->dev, "enable",
						       GPIOD_OUT_LOW);
	if (IS_ERR(charger->enable_gpio))
		return dev_err_probe(charger->dev, PTR_ERR(charger->enable_gpio),
				     "failed to get charger-enable GPIO\n");

	if (!device_property_read_u32(charger->dev,
				      "input-current-limit-microamp", &value))
		charger->dcp_input_current_ua = value;
	if (charger->dcp_input_current_ua < SM5703_INPUT_CURRENT_MIN_UA ||
	    charger->dcp_input_current_ua > SM5703_INPUT_CURRENT_MAX_UA)
		return dev_err_probe(charger->dev, -EINVAL,
				     "DCP input current limit out of range\n");

	if (!device_property_read_u32(charger->dev,
				      "input-voltage-limit-microvolt", &value))
		charger->aicl_voltage_uv = value;
	if (charger->aicl_voltage_uv < SM5703_AICL_VOLTAGE_MIN_UV ||
	    charger->aicl_voltage_uv > SM5703_AICL_VOLTAGE_MAX_UV)
		return dev_err_probe(charger->dev, -EINVAL,
				     "AICL voltage limit out of range\n");
	if (!device_property_present(charger->dev,
				     "siliconmitus,monitored-fuel-gauge"))
		return dev_err_probe(charger->dev, -EINVAL,
				     "missing monitored fuel gauge\n");
	if (!device_property_read_u32(charger->dev,
				      "siliconmitus,poll-interval-ms", &value))
		charger->monitor_interval_ms = value;
	if (charger->monitor_interval_ms < 1000 ||
	    charger->monitor_interval_ms > 60000)
		return dev_err_probe(charger->dev, -EINVAL,
				     "temperature poll interval out of range\n");

	psy_config.drv_data = charger;
	psy_config.fwnode = dev_fwnode(charger->dev);
	charger->psy = devm_power_supply_register(charger->dev,
						  &sm5703_charger_desc,
						  &psy_config);
	if (IS_ERR(charger->psy))
		return dev_err_probe(charger->dev, PTR_ERR(charger->psy),
				     "failed to register power supply\n");

	ret = sm5703_get_battery_info(charger);
	if (ret)
		return ret;

	ret = devm_work_autocancel(charger->dev, &charger->extcon_work,
				   sm5703_extcon_work);
	if (ret)
		return ret;

	ret = devm_delayed_work_autocancel(charger->dev, &charger->aicl_work,
					   sm5703_aicl_work);
	if (ret)
		return ret;
	ret = devm_delayed_work_autocancel(charger->dev,
					   &charger->monitor_work,
					   sm5703_monitor_work);
	if (ret)
		return ret;

	ret = sm5703_hw_init(charger);
	if (ret)
		return dev_err_probe(charger->dev, ret,
				     "failed to initialize charger\n");

	ret = sm5703_request_irqs(pdev, charger);
	if (ret)
		return dev_err_probe(charger->dev, ret,
				     "failed to request charger IRQs\n");

	ret = sm5703_find_extcon(charger);
	if (ret)
		return ret;

	charger->extcon_nb.notifier_call = sm5703_extcon_notifier;
	ret = devm_extcon_register_notifier_all(charger->dev, charger->extcon,
						&charger->extcon_nb);
	if (ret)
		return dev_err_probe(charger->dev, ret,
				     "failed to register extcon notifier\n");

	/* Handle a cable that was already attached before this driver probed. */
	mod_delayed_work(system_wq, &charger->monitor_work, 0);
	schedule_work(&charger->extcon_work);

	return 0;
}

static const struct of_device_id sm5703_charger_of_match[] = {
	{ .compatible = "siliconmitus,sm5703-charger" },
	{ }
};
MODULE_DEVICE_TABLE(of, sm5703_charger_of_match);

static const struct platform_device_id sm5703_charger_id[] = {
	{ "sm5703-charger" },
	{ }
};
MODULE_DEVICE_TABLE(platform, sm5703_charger_id);

static struct platform_driver sm5703_charger_driver = {
	.driver = {
		.name = "sm5703-charger",
		.of_match_table = sm5703_charger_of_match,
	},
	.probe = sm5703_charger_probe,
	.remove = sm5703_charger_remove,
	.id_table = sm5703_charger_id,
};
module_platform_driver(sm5703_charger_driver);

MODULE_AUTHOR("Sean Hoyt <seanhoyt963@gmail.com>");
MODULE_DESCRIPTION("Silicon Mitus SM5703 battery charger driver");
MODULE_LICENSE("GPL");
