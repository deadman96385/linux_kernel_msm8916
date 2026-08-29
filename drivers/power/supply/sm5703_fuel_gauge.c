// SPDX-License-Identifier: GPL-2.0-only
/* Silicon Mitus SM5703 fuel-gauge driver. */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/limits.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/power_supply.h>
#include <linux/property.h>
#include <linux/regmap.h>

#define SM5703_FG_REG_DEVICE_ID		0x00
#define SM5703_FG_REG_CNTL		0x01
#define SM5703_FG_REG_INT		0x02
#define SM5703_FG_REG_INT_MASK		0x03
#define SM5703_FG_REG_STATUS		0x04
#define SM5703_FG_REG_SOC		0x05
#define SM5703_FG_REG_OCV		0x06
#define SM5703_FG_REG_VOLTAGE		0x07
#define SM5703_FG_REG_CURRENT		0x08
#define SM5703_FG_REG_TEMPERATURE	0x09
#define SM5703_FG_REG_VOLTAGE_ALARM	0x0c
#define SM5703_FG_REG_SOC_ALARM		0x0e
#define SM5703_FG_REG_OP_STATUS		0x10
#define SM5703_FG_REG_TOPOFF_SOC	0x12
#define SM5703_FG_REG_PARAM_CTRL	0x13
#define SM5703_FG_REG_PARAM_UPDATE	0x14
#define SM5703_FG_REG_VIT_PERIOD	0x1a
#define SM5703_FG_REG_MIX_RATE		0x1b
#define SM5703_FG_REG_MIX_INIT_BLANK	0x1c
#define SM5703_FG_REG_RCE0		0x20
#define SM5703_FG_REG_DTCD		0x23
#define SM5703_FG_REG_RS		0x24
#define SM5703_FG_REG_RS_MIX_FACTOR	0x25
#define SM5703_FG_REG_RS_MAX		0x26
#define SM5703_FG_REG_RS_MIN		0x27
#define SM5703_FG_REG_VOLT_CAL		0x2b
#define SM5703_FG_REG_CURR_CAL		0x2c
#define SM5703_FG_REG_IOCV_MAN		0x2e
#define SM5703_FG_REG_END_V_IDX		0x2f
#define SM5703_FG_REG_IOCV_L_MIN	0x30
#define SM5703_FG_REG_IOCV_L_MAX	0x37
#define SM5703_FG_REG_IOCV_S_MIN	0x40
#define SM5703_FG_REG_IOCV_S_MAX	0x43
#define SM5703_FG_REG_RESET		0x90
#define SM5703_FG_REG_TABLE_START	0xa0
#define SM5703_FG_MAX_REGISTER		0xcf

#define SM5703_FG_PARAM_UNLOCK		0x3700
#define SM5703_FG_PARAM_LOCK		0x0000
#define SM5703_FG_TABLE_LENGTH		16
#define SM5703_FG_INITIALIZED		0x0007
#define SM5703_FG_RESET_DEFAULT		0x2008
#define SM5703_FG_SW_RESET		0x0008

#define SM5703_FG_CNTL_MIX_MODE		BIT(15)
#define SM5703_FG_CNTL_TEMP_MEASURE	BIT(14)
#define SM5703_FG_CNTL_TOPOFF_SOC	BIT(13)
#define SM5703_FG_CNTL_MANUAL_OCV	BIT(10)
#define SM5703_FG_CNTL_LOW_SOC_IRQ	BIT(3)
#define SM5703_FG_CNTL_LOW_VOLT_IRQ	BIT(0)

#define SM5703_FG_VALUE_MASK		GENMASK(10, 0)
#define SM5703_FG_SIGN_BIT		BIT(15)

#define SM5703_FG_IOCV_AVG_DIFF_MIN	0x29
#define SM5703_FG_IOCV_L_SPREAD_MAX	0x200

struct sm5703_fg_model {
	u16 table[3][SM5703_FG_TABLE_LENGTH];
	u16 rce[3];
	u16 dtcd;
	u16 rs[4];
	u16 vit_period;
	u16 mix[2];
	u16 topoff_soc[2];
	u16 volt_cal;
	u16 curr_cal;
	u16 temp_std;
	u16 temp_offset;
	u16 temp_offset_cal;
	u16 charge_offset_cal;
};

struct sm5703_fg {
	struct device *dev;
	struct regmap *regmap;
	struct power_supply *psy;
	struct sm5703_fg_model model;
	/* Protects model updates and multi-register measurement sequences. */
	struct mutex lock;
	int charge_full_design_uah;
	int voltage_min_design_uv;
	int voltage_max_design_uv;
	int technology;
	int alert_soc;
	int alert_voltage_uv;
};

static const struct regmap_config sm5703_fg_regmap_config = {
	.reg_bits = 8,
	.val_bits = 16,
	.val_format_endian = REGMAP_ENDIAN_LITTLE,
	.max_register = SM5703_FG_MAX_REGISTER,
};

static int sm5703_fg_read(struct sm5703_fg *fg, unsigned int reg,
			  unsigned int *value)
{
	return regmap_read(fg->regmap, reg, value);
}

static int sm5703_fg_write(struct sm5703_fg *fg, unsigned int reg, u16 value)
{
	return regmap_write(fg->regmap, reg, value);
}

static int sm5703_fg_read_voltage(struct sm5703_fg *fg, unsigned int reg,
				  int *uv)
{
	unsigned int raw;
	int ret;

	ret = sm5703_fg_read(fg, reg, &raw);
	if (ret)
		return ret;

	*uv = DIV_ROUND_CLOSEST((raw & SM5703_FG_VALUE_MASK) * 1000000,
				256);
	return 0;
}

static int sm5703_fg_read_current(struct sm5703_fg *fg, int *ua)
{
	unsigned int raw;
	int value;
	int ret;

	ret = sm5703_fg_read(fg, SM5703_FG_REG_CURRENT, &raw);
	if (ret)
		return ret;

	value = DIV_ROUND_CLOSEST((raw & SM5703_FG_VALUE_MASK) * 1000000,
				  256);
	*ua = raw & SM5703_FG_SIGN_BIT ? -value : value;
	return 0;
}

static int sm5703_fg_read_temperature(struct sm5703_fg *fg, int *decic)
{
	unsigned int raw;
	int value;
	int ret;

	ret = sm5703_fg_read(fg, SM5703_FG_REG_TEMPERATURE, &raw);
	if (ret)
		return ret;

	value = DIV_ROUND_CLOSEST((raw & GENMASK(14, 0)) * 10, 256);
	*decic = raw & SM5703_FG_SIGN_BIT ? -value : value;
	return 0;
}

static int sm5703_fg_read_soc(struct sm5703_fg *fg, int *capacity)
{
	unsigned int raw;
	int ret;

	ret = sm5703_fg_read(fg, SM5703_FG_REG_SOC, &raw);
	if (ret)
		return ret;

	*capacity = clamp(DIV_ROUND_CLOSEST(raw, 256), 0, 100);
	return 0;
}

static int sm5703_fg_trimmed_average(struct sm5703_fg *fg,
				     unsigned int first,
				     unsigned int last, int *average,
				     int *spread)
{
	unsigned int value;
	int min = INT_MAX;
	int max = INT_MIN;
	int sum = 0;
	int count = 0;
	int ret;

	for (; first <= last; first++) {
		ret = sm5703_fg_read(fg, first, &value);
		if (ret)
			return ret;

		min = min_t(int, min, value);
		max = max_t(int, max, value);
		sum += value;
		count++;
	}

	if (count <= 2)
		return -EINVAL;

	*average = (sum - min - max) / (count - 2);
	*spread = max - min;
	return 0;
}

static int sm5703_fg_calculate_iocv(struct sm5703_fg *fg, u16 *iocv)
{
	unsigned int status;
	int large, small = 0;
	int large_spread;
	int unused;
	int ret;

	ret = sm5703_fg_trimmed_average(fg, SM5703_FG_REG_IOCV_L_MIN,
					SM5703_FG_REG_IOCV_L_MAX, &large,
					&large_spread);
	if (ret)
		return ret;

	ret = sm5703_fg_read(fg, SM5703_FG_REG_END_V_IDX, &status);
	if (ret)
		return ret;

	if ((status & 0x0030) == 0x0030) {
		ret = sm5703_fg_trimmed_average(fg,
						SM5703_FG_REG_IOCV_S_MIN,
						SM5703_FG_REG_IOCV_S_MAX,
						&small, &unused);
		if (ret)
			return ret;
	}

	if (!small ||
	    (abs(large - small) > SM5703_FG_IOCV_AVG_DIFF_MIN &&
	     large_spread < SM5703_FG_IOCV_L_SPREAD_MAX))
		*iocv = large;
	else
		*iocv = small;

	return 0;
}

static int sm5703_fg_write_model(struct sm5703_fg *fg, bool use_current_ocv)
{
	struct sm5703_fg_model *model = &fg->model;
	unsigned int control;
	u16 iocv;
	int ret, i, j;

	if (use_current_ocv) {
		int ocv_uv;

		ret = sm5703_fg_read_voltage(fg, SM5703_FG_REG_OCV,
					     &ocv_uv);
		if (ret)
			return ret;

		/* IOCV_MAN uses 1/2048 V units, unlike the 1/256 V OCV. */
		iocv = DIV_ROUND_CLOSEST_ULL((u64)ocv_uv * 256, 125000);
	}

	ret = sm5703_fg_write(fg, SM5703_FG_REG_PARAM_CTRL,
			      SM5703_FG_PARAM_UNLOCK);
	if (ret)
		return ret;

	for (i = 0; i < ARRAY_SIZE(model->rce); i++) {
		ret = sm5703_fg_write(fg, SM5703_FG_REG_RCE0 + i,
				      model->rce[i]);
		if (ret)
			goto out_lock;
	}

	ret = sm5703_fg_write(fg, SM5703_FG_REG_DTCD, model->dtcd);
	if (ret)
		goto out_lock;
	ret = sm5703_fg_write(fg, SM5703_FG_REG_RS, model->rs[0]);
	if (ret)
		goto out_lock;
	ret = sm5703_fg_write(fg, SM5703_FG_REG_VIT_PERIOD,
			      model->vit_period);
	if (ret)
		goto out_lock;

	ret = sm5703_fg_write(fg, SM5703_FG_REG_PARAM_CTRL,
			      SM5703_FG_PARAM_UNLOCK |
			      (SM5703_FG_TABLE_LENGTH - 1));
	if (ret)
		goto out_lock;

	for (i = 0; i < ARRAY_SIZE(model->table); i++) {
		for (j = 0; j < SM5703_FG_TABLE_LENGTH; j++) {
			ret = sm5703_fg_write(fg,
					      SM5703_FG_REG_TABLE_START +
					      i * SM5703_FG_TABLE_LENGTH + j,
					      model->table[i][j]);
			if (ret)
				goto out_lock;
		}
	}

	ret = sm5703_fg_write(fg, SM5703_FG_REG_RS_MIX_FACTOR,
			      model->rs[1]);
	if (ret)
		goto out_lock;
	ret = sm5703_fg_write(fg, SM5703_FG_REG_RS_MAX, model->rs[2]);
	if (ret)
		goto out_lock;
	ret = sm5703_fg_write(fg, SM5703_FG_REG_RS_MIN, model->rs[3]);
	if (ret)
		goto out_lock;
	ret = sm5703_fg_write(fg, SM5703_FG_REG_MIX_RATE, model->mix[0]);
	if (ret)
		goto out_lock;
	ret = sm5703_fg_write(fg, SM5703_FG_REG_MIX_INIT_BLANK,
			      model->mix[1]);
	if (ret)
		goto out_lock;
	ret = sm5703_fg_write(fg, SM5703_FG_REG_VOLT_CAL, model->volt_cal);
	if (ret)
		goto out_lock;
	ret = sm5703_fg_write(fg, SM5703_FG_REG_CURR_CAL, model->curr_cal);
	if (ret)
		goto out_lock;
	ret = sm5703_fg_write(fg, SM5703_FG_REG_TOPOFF_SOC,
			      model->topoff_soc[1]);
	if (ret)
		goto out_lock;

	ret = sm5703_fg_read(fg, SM5703_FG_REG_CNTL, &control);
	if (ret)
		goto out_lock;
	control &= ~SM5703_FG_CNTL_TOPOFF_SOC;
	control |= SM5703_FG_CNTL_MIX_MODE |
		   SM5703_FG_CNTL_TEMP_MEASURE |
		   SM5703_FG_CNTL_MANUAL_OCV;
	if (model->topoff_soc[0])
		control |= SM5703_FG_CNTL_TOPOFF_SOC;

	ret = sm5703_fg_write(fg, SM5703_FG_REG_CNTL, control);
	if (ret)
		goto out_lock;

	ret = sm5703_fg_write(fg, SM5703_FG_REG_PARAM_CTRL,
			      SM5703_FG_PARAM_LOCK |
			      (SM5703_FG_TABLE_LENGTH - 1));
	if (ret)
		return ret;

	if (!use_current_ocv) {
		ret = sm5703_fg_calculate_iocv(fg, &iocv);
		if (ret)
			return ret;
	}

	return sm5703_fg_write(fg, SM5703_FG_REG_IOCV_MAN, iocv);

out_lock:
	sm5703_fg_write(fg, SM5703_FG_REG_PARAM_CTRL,
			SM5703_FG_PARAM_LOCK | (SM5703_FG_TABLE_LENGTH - 1));
	return ret;
}

static int sm5703_fg_model_matches(struct sm5703_fg *fg, bool *matches)
{
	struct sm5703_fg_model *model = &fg->model;
	struct sm5703_fg_reg_value {
		unsigned int reg;
		u16 value;
	} values[] = {
		{ SM5703_FG_REG_RCE0, model->rce[0] },
		{ SM5703_FG_REG_RCE0 + 1, model->rce[1] },
		{ SM5703_FG_REG_RCE0 + 2, model->rce[2] },
		{ SM5703_FG_REG_DTCD, model->dtcd },
		{ SM5703_FG_REG_RS, model->rs[0] },
		{ SM5703_FG_REG_RS_MIX_FACTOR, model->rs[1] },
		{ SM5703_FG_REG_RS_MAX, model->rs[2] },
		{ SM5703_FG_REG_RS_MIN, model->rs[3] },
		{ SM5703_FG_REG_VIT_PERIOD, model->vit_period },
		{ SM5703_FG_REG_MIX_RATE, model->mix[0] },
		{ SM5703_FG_REG_MIX_INIT_BLANK, model->mix[1] },
		{ SM5703_FG_REG_VOLT_CAL, model->volt_cal },
	};
	unsigned int value;
	int ret, i, j;

	*matches = false;
	for (i = 0; i < ARRAY_SIZE(values); i++) {
		ret = sm5703_fg_read(fg, values[i].reg, &value);
		if (ret)
			return ret;
		if (value != values[i].value)
			return 0;
	}

	ret = sm5703_fg_read(fg, SM5703_FG_REG_TOPOFF_SOC, &value);
	if (ret)
		return ret;
	/* The upper byte reads back as zero on SM5703 after a 16-bit write. */
	if ((value & 0xff) != (model->topoff_soc[1] & 0xff))
		return 0;

	for (i = 0; i < ARRAY_SIZE(model->table); i++) {
		for (j = 0; j < SM5703_FG_TABLE_LENGTH; j++) {
			ret = sm5703_fg_read(fg,
					     SM5703_FG_REG_TABLE_START +
					     i * SM5703_FG_TABLE_LENGTH + j,
					     &value);
			if (ret)
				return ret;
			if (value != model->table[i][j])
				return 0;
		}
	}

	ret = sm5703_fg_read(fg, SM5703_FG_REG_CNTL, &value);
	if (ret)
		return ret;
	if (!(value & SM5703_FG_CNTL_MIX_MODE) ||
	    !(value & SM5703_FG_CNTL_TEMP_MEASURE) ||
	    !(value & SM5703_FG_CNTL_MANUAL_OCV) ||
	    (!!(value & SM5703_FG_CNTL_TOPOFF_SOC) !=
	     !!model->topoff_soc[0]))
		return 0;

	*matches = true;
	return 0;
}

static int sm5703_fg_ensure_model(struct sm5703_fg *fg, bool verify)
{
	unsigned int control, status;
	bool matches;
	bool reset = false;
	int ret;

	ret = sm5703_fg_read(fg, SM5703_FG_REG_OP_STATUS, &status);
	if (ret)
		return ret;
	if ((status & 0xff) == SM5703_FG_INITIALIZED) {
		if (!verify)
			return 0;

		ret = sm5703_fg_model_matches(fg, &matches);
		if (ret || matches)
			return ret;

		dev_warn(fg->dev,
			 "battery model mismatch, reprogramming fuel gauge\n");
		return sm5703_fg_write_model(fg, true);
	}

	ret = sm5703_fg_read(fg, SM5703_FG_REG_CNTL, &control);
	if (ret)
		return ret;
	if (control == SM5703_FG_RESET_DEFAULT) {
		ret = sm5703_fg_write(fg, SM5703_FG_REG_RESET,
				      SM5703_FG_SW_RESET);
		if (ret)
			return ret;
		msleep(200);
		reset = true;
	}

	return sm5703_fg_write_model(fg, reset);
}

static int sm5703_fg_update_current_calibration(struct sm5703_fg *fg)
{
	union power_supply_propval value;
	int temperature;
	int calibration;
	int current_ua;
	int delta;
	int ret;

	calibration = fg->model.curr_cal;
	ret = power_supply_get_property_from_supplier(fg->psy,
						      POWER_SUPPLY_PROP_STATUS,
						      &value);
	if (!ret && value.intval == POWER_SUPPLY_STATUS_CHARGING) {
		ret = sm5703_fg_read_current(fg, &current_ua);
		if (!ret && current_ua > 30000)
			calibration += fg->model.charge_offset_cal << 8;
	}

	ret = sm5703_fg_read_temperature(fg, &temperature);
	if (ret)
		return ret;

	delta = fg->model.temp_std - temperature / 10;
	delta = delta / fg->model.temp_offset;
	calibration += delta * fg->model.temp_offset_cal * 256;

	return sm5703_fg_write(fg, SM5703_FG_REG_CURR_CAL, calibration);
}

static int sm5703_fg_init_alerts(struct sm5703_fg *fg)
{
	unsigned int unused;
	unsigned int control;
	u16 voltage;
	int ret;

	ret = sm5703_fg_read(fg, SM5703_FG_REG_INT, &unused);
	if (ret)
		return ret;
	ret = sm5703_fg_read(fg, SM5703_FG_REG_STATUS, &unused);
	if (ret)
		return ret;
	ret = sm5703_fg_write(fg, SM5703_FG_REG_INT_MASK, 0);
	if (ret)
		return ret;

	voltage = DIV_ROUND_CLOSEST(fg->alert_voltage_uv * 256ULL, 1000000);
	ret = sm5703_fg_write(fg, SM5703_FG_REG_VOLTAGE_ALARM, voltage);
	if (ret)
		return ret;
	ret = sm5703_fg_write(fg, SM5703_FG_REG_SOC_ALARM,
			      fg->alert_soc << 8);
	if (ret)
		return ret;
	ret = sm5703_fg_write(fg, SM5703_FG_REG_PARAM_UPDATE, 0);
	if (ret)
		return ret;
	ret = sm5703_fg_write(fg, SM5703_FG_REG_PARAM_UPDATE, 1);
	if (ret)
		return ret;

	ret = sm5703_fg_read(fg, SM5703_FG_REG_CNTL, &control);
	if (ret)
		return ret;
	control &= ~GENMASK(3, 0);
	control |= SM5703_FG_CNTL_LOW_SOC_IRQ |
		   SM5703_FG_CNTL_LOW_VOLT_IRQ;
	return sm5703_fg_write(fg, SM5703_FG_REG_CNTL, control);
}

static int sm5703_fg_get_supplier_property(struct sm5703_fg *fg,
					   enum power_supply_property psp,
					   union power_supply_propval *value)
{
	return power_supply_get_property_from_supplier(fg->psy, psp, value);
}

static enum power_supply_property sm5703_fg_properties[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CAPACITY_ALERT_MIN,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_OCV,
	POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN,
	POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_MANUFACTURER,
};

static int sm5703_fg_get_property(struct power_supply *psy,
				  enum power_supply_property psp,
				  union power_supply_propval *value)
{
	struct sm5703_fg *fg = power_supply_get_drvdata(psy);
	int ret = 0;

	mutex_lock(&fg->lock);
	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
	case POWER_SUPPLY_PROP_HEALTH:
	case POWER_SUPPLY_PROP_PRESENT:
		ret = sm5703_fg_get_supplier_property(fg, psp, value);
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		value->intval = fg->technology;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		/* A full table comparison is done at probe; poll reset state here. */
		ret = sm5703_fg_ensure_model(fg, false);
		if (!ret)
			ret = sm5703_fg_update_current_calibration(fg);
		if (!ret)
			ret = sm5703_fg_read_soc(fg, &value->intval);
		break;
	case POWER_SUPPLY_PROP_CAPACITY_ALERT_MIN:
		value->intval = fg->alert_soc;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		ret = sm5703_fg_read_voltage(fg, SM5703_FG_REG_VOLTAGE,
					     &value->intval);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_OCV:
		ret = sm5703_fg_read_voltage(fg, SM5703_FG_REG_OCV,
					     &value->intval);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN:
		value->intval = fg->voltage_min_design_uv;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN:
		value->intval = fg->voltage_max_design_uv;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		ret = sm5703_fg_read_current(fg, &value->intval);
		break;
	case POWER_SUPPLY_PROP_TEMP:
		ret = sm5703_fg_read_temperature(fg, &value->intval);
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		value->intval = fg->charge_full_design_uah;
		break;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		value->strval = "SM5703";
		break;
	case POWER_SUPPLY_PROP_MANUFACTURER:
		value->strval = "Silicon Mitus";
		break;
	default:
		ret = -EINVAL;
		break;
	}
	mutex_unlock(&fg->lock);

	return ret;
}

static const struct power_supply_desc sm5703_fg_desc = {
	.name = "sm5703-battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = sm5703_fg_properties,
	.num_properties = ARRAY_SIZE(sm5703_fg_properties),
	.get_property = sm5703_fg_get_property,
};

static irqreturn_t sm5703_fg_irq_thread(int irq, void *data)
{
	struct sm5703_fg *fg = data;
	unsigned int interrupts, status;
	int ret;

	mutex_lock(&fg->lock);
	ret = sm5703_fg_read(fg, SM5703_FG_REG_INT, &interrupts);
	if (!ret)
		ret = sm5703_fg_read(fg, SM5703_FG_REG_STATUS, &status);
	mutex_unlock(&fg->lock);
	if (ret)
		dev_err_ratelimited(fg->dev, "failed to read alert: %d\n", ret);
	else
		dev_dbg(fg->dev, "fuel-gauge alert: int=%#x status=%#x\n",
			interrupts, status);

	pm_wakeup_dev_event(fg->dev, 2000, true);
	power_supply_changed(fg->psy);
	return IRQ_HANDLED;
}

static int sm5703_fg_read_u16_array(struct device *dev, const char *property,
				    u16 *destination, size_t count)
{
	u32 values[SM5703_FG_TABLE_LENGTH];
	int ret, i;

	if (count > ARRAY_SIZE(values))
		return -EINVAL;

	ret = device_property_read_u32_array(dev, property, values, count);
	if (ret)
		return dev_err_probe(dev, ret, "missing %s\n", property);

	for (i = 0; i < count; i++) {
		if (values[i] > U16_MAX)
			return dev_err_probe(dev, -ERANGE,
					     "%s value out of range\n", property);
		destination[i] = values[i];
	}

	return 0;
}

static int sm5703_fg_read_u16(struct device *dev, const char *property,
			      u16 *destination)
{
	u32 value;
	int ret;

	ret = device_property_read_u32(dev, property, &value);
	if (ret)
		return dev_err_probe(dev, ret, "missing %s\n", property);
	if (value > U16_MAX)
		return dev_err_probe(dev, -ERANGE, "%s value out of range\n",
				     property);

	*destination = value;
	return 0;
}

static int sm5703_fg_parse_model(struct sm5703_fg *fg)
{
	struct sm5703_fg_model *model = &fg->model;
	static const char * const table_properties[] = {
		"siliconmitus,battery-table-0",
		"siliconmitus,battery-table-1",
		"siliconmitus,battery-table-2",
	};
	int ret, i;

	for (i = 0; i < ARRAY_SIZE(table_properties); i++) {
		ret = sm5703_fg_read_u16_array(fg->dev, table_properties[i],
					       model->table[i],
					       SM5703_FG_TABLE_LENGTH);
		if (ret)
			return ret;
	}

	ret = sm5703_fg_read_u16_array(fg->dev, "siliconmitus,rce-values",
				       model->rce, ARRAY_SIZE(model->rce));
	if (ret)
		return ret;
	ret = sm5703_fg_read_u16(fg->dev, "siliconmitus,dtcd-value",
				 &model->dtcd);
	if (ret)
		return ret;
	ret = sm5703_fg_read_u16_array(fg->dev, "siliconmitus,rs-values",
				       model->rs, ARRAY_SIZE(model->rs));
	if (ret)
		return ret;
	ret = sm5703_fg_read_u16(fg->dev, "siliconmitus,vit-period",
				 &model->vit_period);
	if (ret)
		return ret;
	ret = sm5703_fg_read_u16_array(fg->dev, "siliconmitus,mix-values",
				       model->mix, ARRAY_SIZE(model->mix));
	if (ret)
		return ret;
	ret = sm5703_fg_read_u16_array(fg->dev,
				       "siliconmitus,topoff-soc-values",
				       model->topoff_soc,
				       ARRAY_SIZE(model->topoff_soc));
	if (ret)
		return ret;
	ret = sm5703_fg_read_u16(fg->dev, "siliconmitus,voltage-calibration",
				 &model->volt_cal);
	if (ret)
		return ret;
	ret = sm5703_fg_read_u16(fg->dev, "siliconmitus,current-calibration",
				 &model->curr_cal);
	if (ret)
		return ret;
	ret = sm5703_fg_read_u16(fg->dev,
				 "siliconmitus,temperature-standard-celsius",
				 &model->temp_std);
	if (ret)
		return ret;
	ret = sm5703_fg_read_u16(fg->dev,
				 "siliconmitus,temperature-offset-divisor",
				 &model->temp_offset);
	if (ret)
		return ret;
	if (!model->temp_offset)
		return dev_err_probe(fg->dev, -EINVAL,
				     "temperature offset divisor must be nonzero\n");
	ret = sm5703_fg_read_u16(fg->dev,
				 "siliconmitus,temperature-offset-calibration",
				 &model->temp_offset_cal);
	if (ret)
		return ret;
	return sm5703_fg_read_u16(fg->dev,
				  "siliconmitus,charge-offset-calibration",
				  &model->charge_offset_cal);
}

static int sm5703_fg_get_battery_info(struct sm5703_fg *fg)
{
	struct power_supply_battery_info *info;
	int ret;

	ret = power_supply_get_battery_info(fg->psy, &info);
	if (ret)
		return dev_err_probe(fg->dev, ret,
				     "failed to get battery information\n");

	fg->charge_full_design_uah = info->charge_full_design_uah;
	fg->voltage_min_design_uv = info->voltage_min_design_uv;
	fg->voltage_max_design_uv = info->voltage_max_design_uv;
	fg->technology = info->technology;
	power_supply_put_battery_info(fg->psy, info);
	return 0;
}

static int sm5703_fg_probe(struct i2c_client *client)
{
	struct power_supply_config psy_config = { };
	struct device *dev = &client->dev;
	struct sm5703_fg *fg;
	unsigned int id;
	u32 value;
	int ret;

	fg = devm_kzalloc(dev, sizeof(*fg), GFP_KERNEL);
	if (!fg)
		return -ENOMEM;

	fg->dev = dev;
	fg->alert_soc = 5;
	fg->alert_voltage_uv = 3125000;
	mutex_init(&fg->lock);
	i2c_set_clientdata(client, fg);

	fg->regmap = devm_regmap_init_i2c(client, &sm5703_fg_regmap_config);
	if (IS_ERR(fg->regmap))
		return dev_err_probe(dev, PTR_ERR(fg->regmap),
				     "failed to initialize regmap\n");

	ret = sm5703_fg_read(fg, SM5703_FG_REG_DEVICE_ID, &id);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read device ID\n");
	dev_info(dev, "SM5703 fuel gauge detected (device ID %#x)\n", id);

	ret = sm5703_fg_parse_model(fg);
	if (ret)
		return ret;

	if (!device_property_read_u32(dev, "siliconmitus,fuel-alert-soc",
				      &value))
		fg->alert_soc = value;
	if (fg->alert_soc < 0 || fg->alert_soc > 100)
		return dev_err_probe(dev, -EINVAL, "invalid fuel alert SOC\n");

	if (!device_property_read_u32(dev,
				      "siliconmitus,voltage-alert-microvolt",
				      &value))
		fg->alert_voltage_uv = value;
	if (fg->alert_voltage_uv < 2500000 || fg->alert_voltage_uv > 5000000)
		return dev_err_probe(dev, -EINVAL,
				     "invalid voltage alert threshold\n");

	psy_config.drv_data = fg;
	psy_config.fwnode = dev_fwnode(dev);
	fg->psy = devm_power_supply_register(dev, &sm5703_fg_desc, &psy_config);
	if (IS_ERR(fg->psy))
		return dev_err_probe(dev, PTR_ERR(fg->psy),
				     "failed to register power supply\n");

	ret = sm5703_fg_get_battery_info(fg);
	if (ret)
		return ret;

	mutex_lock(&fg->lock);
	ret = sm5703_fg_ensure_model(fg, true);
	if (!ret)
		ret = sm5703_fg_init_alerts(fg);
	mutex_unlock(&fg->lock);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to initialize fuel gauge\n");

	if (client->irq > 0) {
		ret = devm_request_threaded_irq(dev, client->irq, NULL,
						sm5703_fg_irq_thread,
						IRQF_ONESHOT, "sm5703-fg", fg);
		if (ret)
			return dev_err_probe(dev, ret,
					     "failed to request interrupt\n");
	}

	return 0;
}

static const struct of_device_id sm5703_fg_of_match[] = {
	{ .compatible = "siliconmitus,sm5703-fuel-gauge" },
	{ }
};
MODULE_DEVICE_TABLE(of, sm5703_fg_of_match);

static const struct i2c_device_id sm5703_fg_id[] = {
	{ "sm5703-fuel-gauge" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, sm5703_fg_id);

static struct i2c_driver sm5703_fg_driver = {
	.driver = {
		.name = "sm5703-fuel-gauge",
		.of_match_table = sm5703_fg_of_match,
	},
	.probe = sm5703_fg_probe,
	.id_table = sm5703_fg_id,
};
module_i2c_driver(sm5703_fg_driver);

MODULE_AUTHOR("Sean Hoyt <seanhoyt963@gmail.com>");
MODULE_DESCRIPTION("Silicon Mitus SM5703 fuel-gauge driver");
MODULE_LICENSE("GPL");
