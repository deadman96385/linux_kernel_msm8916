// SPDX-License-Identifier: GPL-2.0-only
/*
 * Maxim MAX77849 battery charger driver
 *
 * The charger is similar to the MAX77693, but several status bits and the
 * current/voltage encodings are different.  Keep this as a distinct driver so
 * that a MAX77693 fallback cannot silently program an unsafe battery voltage.
 */

#include <linux/device.h>
#include <linux/devm-helpers.h>
#include <linux/err.h>
#include <linux/jiffies.h>
#include <linux/limits.h>
#include <linux/mfd/max77849.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/power_supply.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>

#define MAX77849_CHARGER_NAME			"max77849-charger"
#define MAX77849_MANUFACTURER			"Maxim Integrated"
#define MAX77849_MODEL				"MAX77849"

#define MAX77849_INPUT_CURRENT_STEP_UA		25000
#define MAX77849_INPUT_CURRENT_MIN_UA		100000
#define MAX77849_INPUT_CURRENT_MAX_UA		3175000
#define MAX77849_CHARGE_CURRENT_STEP_UA		40000
#define MAX77849_CHARGE_CURRENT_MIN_UA		40000
#define MAX77849_CHARGE_CURRENT_MAX_UA		2520000

#define MAX77849_DEFAULT_POLL_INTERVAL_MS	5000
#define MAX77849_DEFAULT_MIN_SYSTEM_UV		3600000
#define MAX77849_DEFAULT_FAST_TIMER_HOURS	16
#define MAX77849_DEFAULT_TOPOFF_TIMER_MINUTES	70

enum max77849_thermal_state {
	MAX77849_THERMAL_UNKNOWN,
	MAX77849_THERMAL_NORMAL,
	MAX77849_THERMAL_COLD,
	MAX77849_THERMAL_HOT,
};

struct max77849_current_profile {
	int input_ua;
	int charge_ua;
};

struct max77849_charger {
	struct device *dev;
	struct max77849 *max77849;
	struct regmap *regmap;
	struct power_supply *psy;
	struct delayed_work monitor_work;
	/* Serializes policy, thermal state, and charger register updates. */
	struct mutex lock;

	bool user_enabled;
	bool source_enabled;
	bool hw_charge_enabled;
	bool initialized;
	enum max77849_thermal_state thermal_state;
	int battery_temp_decic;

	int input_current_limit_ua;
	int input_current_limit_max_ua;
	int charge_current_ua;
	int charge_current_max_ua;
	int charge_voltage_uv;
	int charge_voltage_max_uv;
	int overvoltage_limit_uv;
	int charge_term_current_ua;
	int min_system_voltage_uv;
	int charge_restart_voltage_uv;
	int otg_current_limit_ua;
	enum power_supply_usb_type usb_type;
	struct max77849_current_profile sdp_profile;
	struct max77849_current_profile cdp_profile;
	struct max77849_current_profile dcp_profile;

	int temp_stop_min_decic;
	int temp_stop_max_decic;
	int temp_resume_min_decic;
	int temp_resume_max_decic;

	u32 fast_charge_timer_hours;
	u32 topoff_timer_minutes;
	u32 poll_interval_ms;

	bool have_snapshot;
	unsigned int last_int_ok;
	unsigned int last_details_01;
};

static int max77849_unlock(struct max77849_charger *chg)
{
	unsigned int val;
	int ret;

	ret = regmap_update_bits(chg->regmap, MAX77849_REG_CHG_CNFG_06,
				 MAX77849_CHG_CNFG_06_CHGPROT_MASK,
				 MAX77849_CHG_CNFG_06_CHGPROT_UNLOCK);
	if (ret)
		return ret;

	ret = regmap_read(chg->regmap, MAX77849_REG_CHG_CNFG_06, &val);
	if (ret)
		return ret;

	if ((val & MAX77849_CHG_CNFG_06_CHGPROT_MASK) !=
	    MAX77849_CHG_CNFG_06_CHGPROT_UNLOCK)
		return -EACCES;

	return 0;
}

static int max77849_input_current_to_reg(int ua)
{
	ua = clamp(ua, MAX77849_INPUT_CURRENT_MIN_UA,
		   MAX77849_INPUT_CURRENT_MAX_UA);

	return ua / MAX77849_INPUT_CURRENT_STEP_UA;
}

static int max77849_charge_current_to_reg(int ua)
{
	ua = clamp(ua, MAX77849_CHARGE_CURRENT_MIN_UA,
		   MAX77849_CHARGE_CURRENT_MAX_UA);

	return ua / MAX77849_CHARGE_CURRENT_STEP_UA;
}

static int max77849_cv_to_reg(int uv)
{
	if (uv >= 3800000 && uv <= 4325000)
		return 0x06 + (uv - 3800000) / 25000;
	if (uv >= 4325000 && uv < 4340000)
		return 0x1b;
	if (uv >= 4340000 && uv < 4350000)
		return 0x1c;
	if (uv >= 4350000 && uv < 4425000)
		return 0x1d + (uv - 4350000) / 25000;
	if (uv >= 4425000 && uv <= 4550000)
		return (uv - 4425000) / 25000;

	return -EINVAL;
}

static int max77849_reg_to_cv(unsigned int reg)
{
	reg &= MAX77849_CHG_CNFG_04_CV_MASK;

	if (reg <= 0x05)
		return 4425000 + reg * 25000;
	if (reg <= 0x1b)
		return 3650000 + reg * 25000;
	if (reg == 0x1c)
		return 4340000;

	return 3625000 + reg * 25000;
}

static int max77849_topoff_current_to_reg(int ua)
{
	if (ua < 100000 || ua > 350000)
		return -EINVAL;
	if (ua <= 200000)
		return (ua - 100000) / 25000;

	return ua / 50000;
}

static int max77849_restart_to_reg(struct max77849_charger *chg)
{
	int delta_uv = chg->charge_voltage_uv - chg->charge_restart_voltage_uv;

	/* The hardware supports 100, 150 or 200 mV below CV. */
	if (delta_uv <= 100000)
		return 0;
	if (delta_uv <= 150000)
		return 1;

	return 2;
}

static int max77849_set_input_current(struct max77849_charger *chg, int ua)
{
	int reg = max77849_input_current_to_reg(ua);
	int ret;

	ret = max77849_unlock(chg);
	if (ret)
		return ret;

	ret = regmap_update_bits(chg->regmap, MAX77849_REG_CHG_CNFG_09,
				 MAX77849_CHG_CNFG_09_CHGIN_ILIM_MASK, reg);
	if (!ret)
		chg->input_current_limit_ua =
			reg * MAX77849_INPUT_CURRENT_STEP_UA;

	return ret;
}

static int max77849_set_charge_current(struct max77849_charger *chg, int ua)
{
	int reg = max77849_charge_current_to_reg(ua);
	int ret;

	ret = max77849_unlock(chg);
	if (ret)
		return ret;

	ret = regmap_update_bits(chg->regmap, MAX77849_REG_CHG_CNFG_02,
				 MAX77849_CHG_CNFG_02_CC_MASK, reg);
	if (!ret)
		chg->charge_current_ua =
			reg * MAX77849_CHARGE_CURRENT_STEP_UA;

	return ret;
}

static int max77849_set_charge_voltage(struct max77849_charger *chg, int uv)
{
	int reg = max77849_cv_to_reg(uv);
	int ret;

	if (reg < 0)
		return reg;

	ret = max77849_unlock(chg);
	if (ret)
		return ret;

	ret = regmap_update_bits(chg->regmap, MAX77849_REG_CHG_CNFG_04,
				 MAX77849_CHG_CNFG_04_CV_MASK, reg);
	if (!ret)
		chg->charge_voltage_uv = max77849_reg_to_cv(reg);

	return ret;
}

static int max77849_apply_charge_enable(struct max77849_charger *chg)
{
	bool enable = chg->user_enabled && chg->source_enabled &&
		      chg->thermal_state == MAX77849_THERMAL_NORMAL;
	int ret;

	ret = max77849_set_charging(chg->max77849, enable);
	if (!ret)
		chg->hw_charge_enabled =
			max77849_charging_active(chg->max77849);

	return ret;
}

static int max77849_get_online(struct max77849_charger *chg, int *online)
{
	unsigned int val;
	int ret;

	ret = regmap_read(chg->regmap, MAX77849_REG_CHG_INT_OK, &val);
	if (!ret)
		*online = !!(val & MAX77849_CHG_INT_OK_CHGIN) &&
			  READ_ONCE(chg->source_enabled);

	return ret;
}

static int max77849_get_present(struct max77849_charger *chg, int *present)
{
	unsigned int val;
	int ret;

	ret = regmap_read(chg->regmap, MAX77849_REG_CHG_INT_OK, &val);
	if (!ret)
		*present = !!(val & MAX77849_CHG_INT_OK_BATP);

	return ret;
}

static int max77849_get_status(struct max77849_charger *chg, int *status)
{
	unsigned int val;
	int online;
	int ret;

	ret = max77849_get_online(chg, &online);
	if (ret)
		return ret;
	if (!online) {
		*status = POWER_SUPPLY_STATUS_DISCHARGING;
		return 0;
	}

	if (READ_ONCE(chg->thermal_state) != MAX77849_THERMAL_NORMAL ||
	    !READ_ONCE(chg->user_enabled) ||
	    !max77849_charging_active(chg->max77849)) {
		*status = POWER_SUPPLY_STATUS_NOT_CHARGING;
		return 0;
	}

	ret = regmap_read(chg->regmap, MAX77849_REG_CHG_DETAILS_01, &val);
	if (ret)
		return ret;

	switch (val & MAX77849_CHG_DETAILS_01_CHG_MASK) {
	case 0x0:
	case 0x1:
	case 0x2:
		*status = POWER_SUPPLY_STATUS_CHARGING;
		break;
	case 0x3:
	case 0x4:
		*status = POWER_SUPPLY_STATUS_FULL;
		break;
	case 0x5:
	case 0x6:
	case 0x7:
		*status = POWER_SUPPLY_STATUS_NOT_CHARGING;
		break;
	case 0x8:
	case 0xa:
	case 0xb:
		*status = POWER_SUPPLY_STATUS_DISCHARGING;
		break;
	default:
		*status = POWER_SUPPLY_STATUS_UNKNOWN;
		break;
	}

	return 0;
}

static int max77849_get_charge_type(struct max77849_charger *chg, int *type)
{
	unsigned int val;
	int ret;

	if (READ_ONCE(chg->thermal_state) != MAX77849_THERMAL_NORMAL ||
	    !READ_ONCE(chg->user_enabled) ||
	    !max77849_charging_active(chg->max77849)) {
		*type = POWER_SUPPLY_CHARGE_TYPE_NONE;
		return 0;
	}

	ret = regmap_read(chg->regmap, MAX77849_REG_CHG_DETAILS_01, &val);
	if (ret)
		return ret;

	switch (val & MAX77849_CHG_DETAILS_01_CHG_MASK) {
	case 0x0:
		*type = POWER_SUPPLY_CHARGE_TYPE_TRICKLE;
		break;
	case 0x1:
	case 0x2:
	case 0x3:
		*type = POWER_SUPPLY_CHARGE_TYPE_FAST;
		break;
	default:
		*type = POWER_SUPPLY_CHARGE_TYPE_NONE;
		break;
	}

	return 0;
}

static int max77849_get_health(struct max77849_charger *chg, int *health)
{
	enum max77849_thermal_state thermal = READ_ONCE(chg->thermal_state);
	unsigned int val;
	unsigned int state;
	int ret;

	if (thermal == MAX77849_THERMAL_COLD) {
		*health = POWER_SUPPLY_HEALTH_COLD;
		return 0;
	}
	if (thermal == MAX77849_THERMAL_HOT) {
		*health = POWER_SUPPLY_HEALTH_OVERHEAT;
		return 0;
	}
	if (thermal == MAX77849_THERMAL_UNKNOWN) {
		*health = POWER_SUPPLY_HEALTH_UNKNOWN;
		return 0;
	}

	ret = regmap_read(chg->regmap, MAX77849_REG_CHG_DETAILS_00, &val);
	if (ret)
		return ret;
	state = (val & MAX77849_CHG_DETAILS_00_CHGIN_MASK) >>
		MAX77849_CHG_DETAILS_00_CHGIN_SHIFT;
	if (state == 0x2) {
		*health = POWER_SUPPLY_HEALTH_OVERVOLTAGE;
		return 0;
	}

	ret = regmap_read(chg->regmap, MAX77849_REG_CHG_DETAILS_01, &val);
	if (ret)
		return ret;

	state = val & MAX77849_CHG_DETAILS_01_CHG_MASK;
	if (state == 0x6) {
		*health = POWER_SUPPLY_HEALTH_SAFETY_TIMER_EXPIRE;
		return 0;
	}
	if (state == 0xb) {
		*health = POWER_SUPPLY_HEALTH_WATCHDOG_TIMER_EXPIRE;
		return 0;
	}

	switch ((val & MAX77849_CHG_DETAILS_01_BAT_MASK) >>
		MAX77849_CHG_DETAILS_01_BAT_SHIFT) {
	case 0x0:
		*health = POWER_SUPPLY_HEALTH_NO_BATTERY;
		break;
	case 0x1:
	case 0x3:
	case 0x4:
		*health = POWER_SUPPLY_HEALTH_GOOD;
		break;
	case 0x2:
		*health = POWER_SUPPLY_HEALTH_DEAD;
		break;
	case 0x5:
		*health = POWER_SUPPLY_HEALTH_OVERVOLTAGE;
		break;
	default:
		*health = POWER_SUPPLY_HEALTH_UNKNOWN;
		break;
	}

	return 0;
}

static int max77849_read_input_current(struct max77849_charger *chg, int *ua)
{
	unsigned int val;
	int ret;

	ret = regmap_read(chg->regmap, MAX77849_REG_CHG_CNFG_09, &val);
	if (!ret)
		*ua = (val & MAX77849_CHG_CNFG_09_CHGIN_ILIM_MASK) *
		      MAX77849_INPUT_CURRENT_STEP_UA;

	return ret;
}

static int max77849_read_charge_current(struct max77849_charger *chg, int *ua)
{
	unsigned int val;
	int ret;

	ret = regmap_read(chg->regmap, MAX77849_REG_CHG_CNFG_02, &val);
	if (!ret)
		*ua = (val & MAX77849_CHG_CNFG_02_CC_MASK) *
		      MAX77849_CHARGE_CURRENT_STEP_UA;

	return ret;
}

static int max77849_read_charge_voltage(struct max77849_charger *chg, int *uv)
{
	unsigned int val;
	int ret;

	ret = regmap_read(chg->regmap, MAX77849_REG_CHG_CNFG_04, &val);
	if (!ret)
		*uv = max77849_reg_to_cv(val);

	return ret;
}

static enum power_supply_property max77849_charger_properties[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_CHARGE_TYPE,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_USB_TYPE,
	POWER_SUPPLY_PROP_CHARGE_BEHAVIOUR,
	POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_CHARGE_TERM_CURRENT,
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_MANUFACTURER,
};

static int max77849_charger_get_property(struct power_supply *psy,
					 enum power_supply_property psp,
					 union power_supply_propval *val)
{
	struct max77849_charger *chg = power_supply_get_drvdata(psy);

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		return max77849_get_status(chg, &val->intval);
	case POWER_SUPPLY_PROP_CHARGE_TYPE:
		return max77849_get_charge_type(chg, &val->intval);
	case POWER_SUPPLY_PROP_HEALTH:
		return max77849_get_health(chg, &val->intval);
	case POWER_SUPPLY_PROP_PRESENT:
		return max77849_get_present(chg, &val->intval);
	case POWER_SUPPLY_PROP_ONLINE:
		return max77849_get_online(chg, &val->intval);
	case POWER_SUPPLY_PROP_USB_TYPE:
		val->intval = READ_ONCE(chg->usb_type);
		return 0;
	case POWER_SUPPLY_PROP_CHARGE_BEHAVIOUR:
		val->intval = READ_ONCE(chg->user_enabled) ?
			POWER_SUPPLY_CHARGE_BEHAVIOUR_AUTO :
			POWER_SUPPLY_CHARGE_BEHAVIOUR_INHIBIT_CHARGE;
		return 0;
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		return max77849_read_input_current(chg, &val->intval);
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
		return max77849_read_charge_current(chg, &val->intval);
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX:
		val->intval = chg->charge_current_max_ua;
		return 0;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE:
		return max77849_read_charge_voltage(chg, &val->intval);
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE_MAX:
		val->intval = chg->charge_voltage_max_uv;
		return 0;
	case POWER_SUPPLY_PROP_CHARGE_TERM_CURRENT:
		val->intval = chg->charge_term_current_ua;
		return 0;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = MAX77849_MODEL;
		return 0;
	case POWER_SUPPLY_PROP_MANUFACTURER:
		val->strval = MAX77849_MANUFACTURER;
		return 0;
	default:
		return -EINVAL;
	}
}

static int max77849_charger_set_property(struct power_supply *psy,
					 enum power_supply_property psp,
					 const union power_supply_propval *val)
{
	struct max77849_charger *chg = power_supply_get_drvdata(psy);
	const struct max77849_current_profile *profile;
	int ret;

	if (!READ_ONCE(chg->initialized))
		return -EAGAIN;

	mutex_lock(&chg->lock);

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		chg->source_enabled = !!val->intval;
		ret = max77849_apply_charge_enable(chg);
		break;
	case POWER_SUPPLY_PROP_USB_TYPE:
		switch (val->intval) {
		case POWER_SUPPLY_USB_TYPE_SDP:
			profile = &chg->sdp_profile;
			break;
		case POWER_SUPPLY_USB_TYPE_CDP:
			profile = &chg->cdp_profile;
			break;
		case POWER_SUPPLY_USB_TYPE_DCP:
		case POWER_SUPPLY_USB_TYPE_APPLE_BRICK_ID:
			profile = &chg->dcp_profile;
			break;
		case POWER_SUPPLY_USB_TYPE_UNKNOWN:
			profile = &chg->sdp_profile;
			break;
		default:
			ret = -EINVAL;
			goto out_unlock;
		}

		ret = max77849_set_input_current(chg, profile->input_ua);
		if (ret)
			break;
		ret = max77849_set_charge_current(chg, profile->charge_ua);
		if (!ret)
			chg->usb_type = val->intval;
		break;
	case POWER_SUPPLY_PROP_CHARGE_BEHAVIOUR:
		if (val->intval == POWER_SUPPLY_CHARGE_BEHAVIOUR_AUTO) {
			chg->user_enabled = true;
		} else if (val->intval ==
			   POWER_SUPPLY_CHARGE_BEHAVIOUR_INHIBIT_CHARGE) {
			chg->user_enabled = false;
		} else {
			ret = -EINVAL;
			goto out_unlock;
		}
		ret = max77849_apply_charge_enable(chg);
		break;
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		if (val->intval < MAX77849_INPUT_CURRENT_MIN_UA ||
		    val->intval > chg->input_current_limit_max_ua) {
			ret = -EINVAL;
			goto out_unlock;
		}
		ret = max77849_set_input_current(chg, val->intval);
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
		if (val->intval < MAX77849_CHARGE_CURRENT_MIN_UA ||
		    val->intval > chg->charge_current_max_ua) {
			ret = -EINVAL;
			goto out_unlock;
		}
		ret = max77849_set_charge_current(chg, val->intval);
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE:
		if (val->intval > chg->charge_voltage_max_uv) {
			ret = -EINVAL;
			goto out_unlock;
		}
		ret = max77849_set_charge_voltage(chg, val->intval);
		break;
	default:
		ret = -EINVAL;
		goto out_unlock;
	}

out_unlock:
	mutex_unlock(&chg->lock);
	if (!ret)
		power_supply_changed(chg->psy);

	return ret;
}

static int max77849_property_is_writeable(struct power_supply *psy,
					  enum power_supply_property psp)
{
	switch (psp) {
	case POWER_SUPPLY_PROP_CHARGE_BEHAVIOUR:
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE:
		return true;
	default:
		return false;
	}
}

static const struct power_supply_desc max77849_charger_desc = {
	.name = MAX77849_CHARGER_NAME,
	.type = POWER_SUPPLY_TYPE_USB,
	.properties = max77849_charger_properties,
	.num_properties = ARRAY_SIZE(max77849_charger_properties),
	.get_property = max77849_charger_get_property,
	.set_property = max77849_charger_set_property,
	.property_is_writeable = max77849_property_is_writeable,
	.usb_types = BIT(POWER_SUPPLY_USB_TYPE_UNKNOWN) |
		     BIT(POWER_SUPPLY_USB_TYPE_SDP) |
		     BIT(POWER_SUPPLY_USB_TYPE_DCP) |
		     BIT(POWER_SUPPLY_USB_TYPE_CDP) |
		     BIT(POWER_SUPPLY_USB_TYPE_APPLE_BRICK_ID),
	.charge_behaviours = BIT(POWER_SUPPLY_CHARGE_BEHAVIOUR_AUTO) |
			     BIT(POWER_SUPPLY_CHARGE_BEHAVIOUR_INHIBIT_CHARGE),
};

static int max77849_get_fuel_gauge_temp(struct max77849_charger *chg,
					int *temp_decic)
{
	union power_supply_propval val;
	struct power_supply *fuel_gauge;
	int ret;

	fuel_gauge = power_supply_get_by_phandle(chg->dev->of_node,
						 "maxim,monitored-fuel-gauge");
	if (IS_ERR(fuel_gauge))
		return PTR_ERR(fuel_gauge);
	if (!fuel_gauge)
		return -EPROBE_DEFER;

	ret = power_supply_get_property(fuel_gauge, POWER_SUPPLY_PROP_TEMP, &val);
	power_supply_put(fuel_gauge);
	if (ret)
		return ret;

	*temp_decic = val.intval;
	return 0;
}

static enum max77849_thermal_state
max77849_update_thermal_state(struct max77849_charger *chg, int temp_decic)
{
	switch (chg->thermal_state) {
	case MAX77849_THERMAL_COLD:
		if (temp_decic < chg->temp_resume_min_decic)
			return MAX77849_THERMAL_COLD;
		break;
	case MAX77849_THERMAL_HOT:
		if (temp_decic > chg->temp_resume_max_decic)
			return MAX77849_THERMAL_HOT;
		break;
	default:
		break;
	}

	if (temp_decic <= chg->temp_stop_min_decic)
		return MAX77849_THERMAL_COLD;
	if (temp_decic >= chg->temp_stop_max_decic)
		return MAX77849_THERMAL_HOT;

	return MAX77849_THERMAL_NORMAL;
}

static void max77849_monitor_work(struct work_struct *work)
{
	struct max77849_charger *chg =
		container_of(work, struct max77849_charger, monitor_work.work);
	enum max77849_thermal_state old_thermal;
	unsigned int int_ok = 0, details = 0;
	bool old_enabled;
	bool changed = false;
	int temp_decic;
	int ret;

	ret = max77849_get_fuel_gauge_temp(chg, &temp_decic);

	mutex_lock(&chg->lock);
	old_thermal = chg->thermal_state;
	old_enabled = chg->hw_charge_enabled;

	if (ret) {
		chg->thermal_state = MAX77849_THERMAL_UNKNOWN;
	} else {
		chg->battery_temp_decic = temp_decic;
		chg->thermal_state =
			max77849_update_thermal_state(chg, temp_decic);
	}

	if (old_thermal != chg->thermal_state) {
		changed = true;
		if (chg->thermal_state == MAX77849_THERMAL_NORMAL)
			dev_info(chg->dev, "battery temperature safe at %d.%d C\n",
				 temp_decic / 10, abs(temp_decic % 10));
		else if (chg->thermal_state == MAX77849_THERMAL_COLD)
			dev_warn(chg->dev, "charging stopped: battery cold at %d.%d C\n",
				 temp_decic / 10, abs(temp_decic % 10));
		else if (chg->thermal_state == MAX77849_THERMAL_HOT)
			dev_warn(chg->dev, "charging stopped: battery hot at %d.%d C\n",
				 temp_decic / 10, abs(temp_decic % 10));
		else
			dev_warn(chg->dev,
				 "charging stopped: fuel-gauge temperature unavailable (%d)\n",
				 ret);
	}

	ret = max77849_apply_charge_enable(chg);
	if (ret)
		dev_err_ratelimited(chg->dev,
				    "failed to update charger enable: %d\n", ret);
	if (old_enabled != chg->hw_charge_enabled)
		changed = true;

	if (!regmap_read(chg->regmap, MAX77849_REG_CHG_INT_OK, &int_ok) &&
	    !regmap_read(chg->regmap, MAX77849_REG_CHG_DETAILS_01, &details)) {
		if (!chg->have_snapshot || int_ok != chg->last_int_ok ||
		    details != chg->last_details_01)
			changed = true;
		chg->last_int_ok = int_ok;
		chg->last_details_01 = details;
		chg->have_snapshot = true;
	}

	mutex_unlock(&chg->lock);

	if (changed)
		power_supply_changed(chg->psy);

	mod_delayed_work(system_wq, &chg->monitor_work,
			 msecs_to_jiffies(chg->poll_interval_ms));
}

static int max77849_parse_profile(struct max77849_charger *chg)
{
	struct power_supply_battery_info *info;
	u32 val;
	int ret;

	ret = power_supply_get_battery_info(chg->psy, &info);
	if (ret)
		return dev_err_probe(chg->dev, ret,
				     "failed to read monitored-battery profile\n");

	if (info->constant_charge_current_max_ua <= 0 ||
	    info->constant_charge_voltage_max_uv <= 0 ||
	    info->charge_term_current_ua <= 0 ||
	    info->charge_restart_voltage_uv <= 0 ||
	    info->overvoltage_limit_uv <= 0 ||
	    info->temp_min == INT_MIN || info->temp_max == INT_MAX ||
	    info->temp_alert_min == INT_MIN || info->temp_alert_max == INT_MAX) {
		ret = -EINVAL;
		dev_err(chg->dev, "monitored-battery profile is incomplete\n");
		goto out_put_info;
	}

	chg->charge_current_max_ua = min(info->constant_charge_current_max_ua,
					 MAX77849_CHARGE_CURRENT_MAX_UA);
	chg->charge_voltage_max_uv = info->constant_charge_voltage_max_uv;
	chg->charge_voltage_uv = info->constant_charge_voltage_max_uv;
	chg->overvoltage_limit_uv = info->overvoltage_limit_uv;
	chg->charge_term_current_ua = info->charge_term_current_ua;
	chg->charge_restart_voltage_uv = info->charge_restart_voltage_uv;
	chg->temp_stop_min_decic = info->temp_min * 10;
	chg->temp_stop_max_decic = info->temp_max * 10;
	chg->temp_resume_min_decic = info->temp_alert_min * 10;
	chg->temp_resume_max_decic = info->temp_alert_max * 10;

	ret = 0;
out_put_info:
	power_supply_put_battery_info(chg->psy, info);
	if (ret)
		return ret;

	ret = device_property_read_u32(chg->dev,
				       "maxim,input-current-limit-microamp", &val);
	if (ret)
		return dev_err_probe(chg->dev, ret,
				     "missing input current limit\n");
	chg->input_current_limit_ua = val;

	ret = device_property_read_u32(chg->dev,
				       "maxim,input-current-limit-max-microamp", &val);
	if (ret)
		return dev_err_probe(chg->dev, ret,
				     "missing maximum input current limit\n");
	chg->input_current_limit_max_ua =
		min_t(u32, val, MAX77849_INPUT_CURRENT_MAX_UA);

	ret = device_property_read_u32(chg->dev,
				       "maxim,charge-current-limit-microamp", &val);
	if (ret)
		return dev_err_probe(chg->dev, ret,
				     "missing charge current limit\n");
	chg->charge_current_ua = val;
	chg->sdp_profile.input_ua = chg->input_current_limit_ua;
	chg->sdp_profile.charge_ua = chg->charge_current_ua;

	ret = device_property_read_u32(chg->dev,
				       "maxim,cdp-input-current-limit-microamp",
				       &val);
	if (ret)
		return dev_err_probe(chg->dev, ret,
				     "missing CDP input current limit\n");
	chg->cdp_profile.input_ua = val;

	ret = device_property_read_u32(chg->dev,
				       "maxim,cdp-charge-current-limit-microamp",
				       &val);
	if (ret)
		return dev_err_probe(chg->dev, ret,
				     "missing CDP charge current limit\n");
	chg->cdp_profile.charge_ua = val;

	ret = device_property_read_u32(chg->dev,
				       "maxim,dcp-input-current-limit-microamp",
				       &val);
	if (ret)
		return dev_err_probe(chg->dev, ret,
				     "missing DCP input current limit\n");
	chg->dcp_profile.input_ua = val;

	ret = device_property_read_u32(chg->dev,
				       "maxim,dcp-charge-current-limit-microamp",
				       &val);
	if (ret)
		return dev_err_probe(chg->dev, ret,
				     "missing DCP charge current limit\n");
	chg->dcp_profile.charge_ua = val;

	chg->otg_current_limit_ua = 500000;
	device_property_read_u32(chg->dev, "maxim,otg-current-limit-microamp",
				 &chg->otg_current_limit_ua);

	chg->min_system_voltage_uv = MAX77849_DEFAULT_MIN_SYSTEM_UV;
	device_property_read_u32(chg->dev, "maxim,min-system-microvolt",
				 &chg->min_system_voltage_uv);

	chg->fast_charge_timer_hours = MAX77849_DEFAULT_FAST_TIMER_HOURS;
	device_property_read_u32(chg->dev, "maxim,fast-charge-timer-hours",
				 &chg->fast_charge_timer_hours);

	chg->topoff_timer_minutes = MAX77849_DEFAULT_TOPOFF_TIMER_MINUTES;
	device_property_read_u32(chg->dev, "maxim,topoff-timer-minutes",
				 &chg->topoff_timer_minutes);

	chg->poll_interval_ms = MAX77849_DEFAULT_POLL_INTERVAL_MS;
	device_property_read_u32(chg->dev, "maxim,poll-interval-ms",
				 &chg->poll_interval_ms);

	if (chg->input_current_limit_ua < MAX77849_INPUT_CURRENT_MIN_UA ||
	    chg->input_current_limit_ua > chg->input_current_limit_max_ua ||
	    chg->charge_current_ua < MAX77849_CHARGE_CURRENT_MIN_UA ||
	    chg->charge_current_ua > chg->charge_current_max_ua ||
	    chg->cdp_profile.input_ua < MAX77849_INPUT_CURRENT_MIN_UA ||
	    chg->cdp_profile.input_ua > chg->input_current_limit_max_ua ||
	    chg->cdp_profile.charge_ua < MAX77849_CHARGE_CURRENT_MIN_UA ||
	    chg->cdp_profile.charge_ua > chg->charge_current_max_ua ||
	    chg->dcp_profile.input_ua < MAX77849_INPUT_CURRENT_MIN_UA ||
	    chg->dcp_profile.input_ua > chg->input_current_limit_max_ua ||
	    chg->dcp_profile.charge_ua < MAX77849_CHARGE_CURRENT_MIN_UA ||
	    chg->dcp_profile.charge_ua > chg->charge_current_max_ua ||
	    chg->charge_restart_voltage_uv >= chg->charge_voltage_uv ||
	    chg->charge_voltage_uv - chg->charge_restart_voltage_uv > 200000 ||
	    chg->charge_voltage_uv > chg->overvoltage_limit_uv ||
	    (chg->otg_current_limit_ua != 500000 &&
	     chg->otg_current_limit_ua != 1200000) ||
	    max77849_cv_to_reg(chg->charge_voltage_uv) < 0 ||
	    max77849_topoff_current_to_reg(chg->charge_term_current_ua) < 0 ||
	    chg->min_system_voltage_uv < 3000000 ||
	    chg->min_system_voltage_uv > 3700000 ||
	    chg->min_system_voltage_uv % 100000 ||
	    (chg->fast_charge_timer_hours &&
	     (chg->fast_charge_timer_hours < 4 ||
	      chg->fast_charge_timer_hours > 16 ||
	      chg->fast_charge_timer_hours % 2)) ||
	    chg->topoff_timer_minutes > 70 ||
	    chg->topoff_timer_minutes % 10 ||
	    chg->poll_interval_ms < 1000 || chg->poll_interval_ms > 60000 ||
	    chg->temp_stop_min_decic >= chg->temp_resume_min_decic ||
	    chg->temp_resume_min_decic >= chg->temp_resume_max_decic ||
	    chg->temp_resume_max_decic >= chg->temp_stop_max_decic) {
		dev_err(chg->dev, "invalid or unsafe charger profile\n");
		return -EINVAL;
	}

	return 0;
}

static int max77849_hw_init(struct max77849_charger *chg)
{
	unsigned int value;
	int timer_reg;
	int topoff_reg;
	int ret;

	/* Stop charging while protected CC/CV parameters are changed. */
	ret = max77849_set_charging(chg->max77849, false);
	if (ret)
		return ret;
	chg->hw_charge_enabled = false;

	ret = max77849_unlock(chg);
	if (ret)
		return ret;

	timer_reg = chg->fast_charge_timer_hours ?
		(chg->fast_charge_timer_hours - 2) / 2 : 0;
	value = timer_reg |
		(max77849_restart_to_reg(chg) <<
		 MAX77849_CHG_CNFG_01_CHGRSTRT_SHIFT) |
		MAX77849_CHG_CNFG_01_PQEN;
	ret = regmap_update_bits(chg->regmap, MAX77849_REG_CHG_CNFG_01,
				 MAX77849_CHG_CNFG_01_FCHGTIME_MASK |
				 MAX77849_CHG_CNFG_01_CHGRSTRT_MASK |
				 MAX77849_CHG_CNFG_01_PQEN, value);
	if (ret)
		return ret;

	ret = max77849_set_charge_current(chg, chg->charge_current_ua);
	if (ret)
		return ret;

	ret = regmap_update_bits(chg->regmap, MAX77849_REG_CHG_CNFG_02,
				 MAX77849_CHG_CNFG_02_OTG_ILIM,
				 chg->otg_current_limit_ua == 1200000 ?
				 MAX77849_CHG_CNFG_02_OTG_ILIM : 0);
	if (ret)
		return ret;

	topoff_reg = max77849_topoff_current_to_reg(chg->charge_term_current_ua);
	value = topoff_reg |
		(chg->topoff_timer_minutes / 10 <<
		 MAX77849_CHG_CNFG_03_TOTIME_SHIFT);
	ret = regmap_update_bits(chg->regmap, MAX77849_REG_CHG_CNFG_03,
				 MAX77849_CHG_CNFG_03_TOITH_MASK |
				 MAX77849_CHG_CNFG_03_TOTIME_MASK, value);
	if (ret)
		return ret;

	ret = max77849_set_charge_voltage(chg, chg->charge_voltage_uv);
	if (ret)
		return ret;

	value = (chg->min_system_voltage_uv - 3000000) / 100000;
	ret = regmap_update_bits(chg->regmap, MAX77849_REG_CHG_CNFG_04,
				 MAX77849_CHG_CNFG_04_MINVSYS_MASK,
				 value << MAX77849_CHG_CNFG_04_MINVSYS_SHIFT);
	if (ret)
		return ret;

	ret = max77849_set_input_current(chg, chg->input_current_limit_ua);
	if (ret)
		return ret;

	/* Match the vendor's 6 A BAT-to-SYS overcurrent setting. */
	ret = regmap_update_bits(chg->regmap, MAX77849_REG_CHG_CNFG_12,
				 MAX77849_CHG_CNFG_12_B2SOVRC_MASK, 0x7);
	if (ret)
		return ret;

	return 0;
}

static int max77849_charger_probe(struct platform_device *pdev)
{
	struct power_supply_config psy_cfg = {};
	struct max77849_charger *chg;
	int ret;

	chg = devm_kzalloc(&pdev->dev, sizeof(*chg), GFP_KERNEL);
	if (!chg)
		return -ENOMEM;

	chg->dev = &pdev->dev;
	chg->max77849 = dev_get_drvdata(pdev->dev.parent);
	if (!chg->max77849)
		return dev_err_probe(&pdev->dev, -ENODEV,
				     "parent device data unavailable\n");
	chg->regmap = chg->max77849->regmap;
	if (!chg->regmap)
		return dev_err_probe(&pdev->dev, -ENODEV,
				     "parent regmap unavailable\n");

	/* Take over from the bootloader in a known, non-charging state. */
	ret = max77849_set_charging(chg->max77849, false);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to disable bootloader charging\n");

	mutex_init(&chg->lock);
	chg->thermal_state = MAX77849_THERMAL_UNKNOWN;
	chg->user_enabled = true;
	chg->usb_type = POWER_SUPPLY_USB_TYPE_UNKNOWN;
	platform_set_drvdata(pdev, chg);

	psy_cfg.drv_data = chg;
	psy_cfg.of_node = pdev->dev.of_node;
	chg->psy = devm_power_supply_register(&pdev->dev,
					      &max77849_charger_desc,
					      &psy_cfg);
	if (IS_ERR(chg->psy))
		return dev_err_probe(&pdev->dev, PTR_ERR(chg->psy),
				     "failed to register power supply\n");

	ret = max77849_parse_profile(chg);
	if (ret)
		return ret;

	ret = max77849_hw_init(chg);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to initialize charger safely\n");

	ret = devm_delayed_work_autocancel(&pdev->dev, &chg->monitor_work,
					   max77849_monitor_work);
	if (ret)
		return ret;

	WRITE_ONCE(chg->initialized, true);
	mod_delayed_work(system_wq, &chg->monitor_work, 0);

	dev_info(&pdev->dev,
		 "charger profile: input=%duA charge=%duA CV=%duV, temperature=%d..%d C\n",
		 chg->input_current_limit_ua, chg->charge_current_ua,
		 chg->charge_voltage_uv, chg->temp_stop_min_decic / 10,
		 chg->temp_stop_max_decic / 10);

	return 0;
}

static void max77849_charger_remove(struct platform_device *pdev)
{
	struct max77849_charger *chg = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&chg->monitor_work);

	mutex_lock(&chg->lock);
	WRITE_ONCE(chg->initialized, false);
	chg->source_enabled = false;
	chg->user_enabled = false;
	if (max77849_set_charging(chg->max77849, false))
		dev_warn(chg->dev, "failed to disable charging during removal\n");
	chg->hw_charge_enabled = false;
	mutex_unlock(&chg->lock);
}

static int max77849_charger_suspend(struct device *dev)
{
	struct max77849_charger *chg = dev_get_drvdata(dev);

	cancel_delayed_work_sync(&chg->monitor_work);
	return 0;
}

static int max77849_charger_resume(struct device *dev)
{
	struct max77849_charger *chg = dev_get_drvdata(dev);

	mod_delayed_work(system_wq, &chg->monitor_work, 0);
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(max77849_charger_pm_ops,
				max77849_charger_suspend,
				max77849_charger_resume);

static const struct of_device_id max77849_charger_of_match[] = {
	{ .compatible = "maxim,max77849-charger" },
	{ }
};
MODULE_DEVICE_TABLE(of, max77849_charger_of_match);

static struct platform_driver max77849_charger_driver = {
	.driver = {
		.name = "max77849-charger",
		.of_match_table = max77849_charger_of_match,
		.pm = pm_sleep_ptr(&max77849_charger_pm_ops),
	},
	.probe = max77849_charger_probe,
	.remove_new = max77849_charger_remove,
};
module_platform_driver(max77849_charger_driver);

MODULE_AUTHOR("Sean Hoyt <seanhoyt963@gmail.com>");
MODULE_DESCRIPTION("Maxim MAX77849 battery charger driver");
MODULE_LICENSE("GPL");
