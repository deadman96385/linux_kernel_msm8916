// SPDX-License-Identifier: GPL-2.0-only
/*
 * Maxim MAX77849 Micro-USB Interface Controller driver
 *
 * The MUIC performs BC1.2 charger detection, switches D+/D- between the
 * application processor and an open circuit, and detects a grounded ID pin
 * for USB host mode. The hardware interrupt provides wake-capable cable
 * detection, with polling retained as a fallback and fault-recovery path.
 */

#include <linux/device.h>
#include <linux/devm-helpers.h>
#include <linux/extcon-provider.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
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

#define MAX77849_MUIC_DEFAULT_POLL_INTERVAL_MS	1000

enum max77849_muic_state_bit {
	MAX77849_STATE_USB,
	MAX77849_STATE_USB_HOST,
	MAX77849_STATE_SDP,
	MAX77849_STATE_DCP,
	MAX77849_STATE_CDP,
	MAX77849_STATE_FAST,
	MAX77849_STATE_SLOW,
	MAX77849_STATE_COUNT,
};

struct max77849_muic {
	struct device *dev;
	struct max77849 *max77849;
	struct regmap *regmap;
	struct power_supply *charger;
	struct extcon_dev *edev;
	struct delayed_work detect_work;
	/* Serializes detected cable state and related hardware transitions. */
	struct mutex lock;

	unsigned long state;
	u8 last_status1;
	u8 last_status2;
	bool have_status;
	bool status_fault;
	u32 poll_interval_ms;
	int irq;
};

static const unsigned int max77849_muic_cables[] = {
	EXTCON_USB,
	EXTCON_USB_HOST,
	EXTCON_CHG_USB_SDP,
	EXTCON_CHG_USB_DCP,
	EXTCON_CHG_USB_CDP,
	EXTCON_CHG_USB_FAST,
	EXTCON_CHG_USB_SLOW,
	EXTCON_NONE,
};

static int max77849_muic_set_path(struct max77849_muic *muic, bool usb,
				  bool attached)
{
	unsigned int ctrl2;
	int ret;

	ret = regmap_update_bits(muic->regmap, MAX77849_MUIC_REG_CTRL1,
				 MAX77849_MUIC_CTRL1_COM_SW_MASK,
				 usb ? MAX77849_MUIC_CTRL1_SW_USB :
				 MAX77849_MUIC_CTRL1_SW_OPEN);
	if (ret)
		return ret;

	ctrl2 = attached ? MAX77849_MUIC_CTRL2_CPEN :
			   MAX77849_MUIC_CTRL2_LOWPWD;

	return regmap_update_bits(muic->regmap, MAX77849_MUIC_REG_CTRL2,
				  MAX77849_MUIC_CTRL2_CPEN |
				  MAX77849_MUIC_CTRL2_LOWPWD, ctrl2);
}

static int
max77849_muic_set_psy_property(struct max77849_muic *muic,
			       enum power_supply_property prop, int value)
{
	union power_supply_propval val = { .intval = value };

	return power_supply_set_property(muic->charger, prop, &val);
}

static int max77849_muic_set_source(struct max77849_muic *muic,
				    enum power_supply_usb_type type,
				    int input_ua, int charge_ua)
{
	int ret;

	ret = max77849_muic_set_psy_property(muic,
					     POWER_SUPPLY_PROP_USB_TYPE, type);
	if (ret)
		return ret;

	if (input_ua > 0) {
		ret = max77849_muic_set_psy_property(muic,
						     POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
				     input_ua);
		if (ret)
			return ret;
	}

	if (charge_ua > 0)
		return max77849_muic_set_psy_property(muic,
			POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT, charge_ua);

	return 0;
}

static int max77849_muic_publish_state(struct max77849_muic *muic,
				       unsigned long state)
{
	unsigned long changed = muic->state ^ state;
	int i;
	int ret;

	for (i = 0; i < MAX77849_STATE_COUNT; i++) {
		if (!(changed & BIT(i)))
			continue;

		ret = extcon_set_state(muic->edev, max77849_muic_cables[i],
				       !!(state & BIT(i)));
		if (ret)
			return ret;
	}

	for (i = 0; i < MAX77849_STATE_COUNT; i++) {
		if (!(changed & BIT(i)))
			continue;

		ret = extcon_sync(muic->edev, max77849_muic_cables[i]);
		if (ret)
			return ret;
	}

	muic->state = state;
	return 0;
}

static int max77849_muic_enter_safe_state(struct max77849_muic *muic)
{
	int first_error = 0;
	int ret;

	/* Stop sinking or sourcing power before changing the data path. */
	ret = max77849_muic_set_psy_property(muic,
					     POWER_SUPPLY_PROP_ONLINE, false);
	if (ret && !first_error)
		first_error = ret;

	ret = max77849_set_otg(muic->max77849, false);
	if (ret && !first_error)
		first_error = ret;

	ret = max77849_muic_set_source(muic,
				       POWER_SUPPLY_USB_TYPE_UNKNOWN, 0, 0);
	if (ret && !first_error)
		first_error = ret;

	ret = max77849_muic_set_path(muic, false, false);
	if (ret && !first_error)
		first_error = ret;

	ret = max77849_muic_publish_state(muic, 0);
	if (ret && !first_error)
		first_error = ret;

	return first_error;
}

static int max77849_muic_apply_status(struct max77849_muic *muic,
				      u8 status1, u8 status2)
{
	unsigned int chg_type = status2 & MAX77849_MUIC_STATUS2_CHGTYP_MASK;
	unsigned int adc = status1 & MAX77849_MUIC_STATUS1_ADC_MASK;
	unsigned long state = 0;
	bool vbus = status2 & MAX77849_MUIC_STATUS2_VBVOLT;
	bool input_overvoltage = status2 & MAX77849_MUIC_STATUS2_DXOVP;
	bool host = adc == MAX77849_MUIC_ADC_GROUND &&
		    !(status1 & (MAX77849_MUIC_STATUS1_ADCERR |
				 MAX77849_MUIC_STATUS1_ADC1K));
	bool usb_path = false;
	bool attached = false;
	int ret;

	if (status2 & MAX77849_MUIC_STATUS2_CHGDETRUN) {
		ret = max77849_muic_enter_safe_state(muic);
		return ret ?: -EAGAIN;
	}
	if (input_overvoltage) {
		dev_warn(muic->dev, "charger input overvoltage detected\n");
		return max77849_muic_enter_safe_state(muic);
	}

	if (host) {
		usb_path = true;
		attached = true;
		state = BIT(MAX77849_STATE_USB_HOST);

		if (vbus) {
			/* Never leave boost enabled against an external VBUS. */
			ret = max77849_set_otg(muic->max77849, false);
			if (ret)
				return ret;
			ret = max77849_muic_set_source(muic,
						       POWER_SUPPLY_USB_TYPE_SDP, 0, 0);
			if (ret)
				return ret;
			ret = max77849_muic_set_psy_property(muic,
							     POWER_SUPPLY_PROP_ONLINE, true);
			if (ret)
				return ret;
		} else {
			ret = max77849_muic_set_psy_property(muic,
							     POWER_SUPPLY_PROP_ONLINE, false);
			if (ret)
				return ret;
			ret = max77849_muic_set_source(muic,
						       POWER_SUPPLY_USB_TYPE_UNKNOWN, 0, 0);
			if (ret)
				return ret;
			ret = max77849_set_otg(muic->max77849, true);
			if (ret)
				return ret;
		}
	} else {
		ret = max77849_set_otg(muic->max77849, false);
		if (ret)
			return ret;

		if (!vbus) {
			ret = max77849_muic_set_psy_property(muic,
							     POWER_SUPPLY_PROP_ONLINE, false);
			if (ret)
				return ret;
			ret = max77849_muic_set_source(muic,
						       POWER_SUPPLY_USB_TYPE_UNKNOWN, 0, 0);
			if (ret)
				return ret;
			goto publish;
		}

		attached = true;
		switch (chg_type) {
		case MAX77849_MUIC_CHGTYP_USB:
			usb_path = true;
			state = BIT(MAX77849_STATE_USB) |
				BIT(MAX77849_STATE_SDP);
			ret = max77849_muic_set_source(muic,
						       POWER_SUPPLY_USB_TYPE_SDP, 0, 0);
			break;
		case MAX77849_MUIC_CHGTYP_CDP:
			usb_path = true;
			state = BIT(MAX77849_STATE_USB) |
				BIT(MAX77849_STATE_CDP);
			ret = max77849_muic_set_source(muic,
						       POWER_SUPPLY_USB_TYPE_CDP, 0, 0);
			break;
		case MAX77849_MUIC_CHGTYP_DCP:
			state = BIT(MAX77849_STATE_DCP);
			ret = max77849_muic_set_source(muic,
						       POWER_SUPPLY_USB_TYPE_DCP, 0, 0);
			break;
		case MAX77849_MUIC_CHGTYP_500MA:
			state = BIT(MAX77849_STATE_SLOW);
			ret = max77849_muic_set_source(muic,
						       POWER_SUPPLY_USB_TYPE_SDP, 0, 0);
			break;
		case MAX77849_MUIC_CHGTYP_1A:
		case MAX77849_MUIC_CHGTYP_SPECIAL:
			state = BIT(MAX77849_STATE_FAST);
			ret = max77849_muic_set_source(muic,
						       POWER_SUPPLY_USB_TYPE_CDP, 0, 0);
			break;
		case MAX77849_MUIC_CHGTYP_DEAD_BATTERY:
			state = BIT(MAX77849_STATE_SLOW);
			ret = max77849_muic_set_source(muic,
						       POWER_SUPPLY_USB_TYPE_UNKNOWN,
				100000, 40000);
			break;
		case MAX77849_MUIC_CHGTYP_NONE:
		default:
			state = BIT(MAX77849_STATE_SLOW);
			ret = max77849_muic_set_source(muic,
						       POWER_SUPPLY_USB_TYPE_UNKNOWN, 0, 0);
			break;
		}
		if (ret)
			return ret;
		ret = max77849_muic_set_psy_property(muic,
						     POWER_SUPPLY_PROP_ONLINE, true);
		if (ret)
			return ret;
	}

publish:
	ret = max77849_muic_set_path(muic, usb_path, attached);
	if (ret)
		return ret;

	ret = max77849_muic_publish_state(muic, state);
	if (ret)
		return ret;

	dev_info(muic->dev, "cable changed: adc=0x%02x type=%u vbus=%u otg=%u\n",
		 adc, chg_type, vbus, host && !vbus);

	return 0;
}

static void max77849_muic_detect(struct max77849_muic *muic)
{
	u8 status[2];
	u8 interrupts[3];
	int ret;

	ret = regmap_bulk_read(muic->regmap, MAX77849_MUIC_REG_STATUS1,
			       status, ARRAY_SIZE(status));
	if (ret) {
		dev_err_ratelimited(muic->dev,
				    "failed to read MUIC status: %d\n", ret);
		mutex_lock(&muic->lock);
		if (!muic->status_fault) {
			int safe_ret;

			safe_ret = max77849_muic_enter_safe_state(muic);
			if (safe_ret)
				dev_err_ratelimited(muic->dev,
						    "failed to enter safe state: %d\n",
					safe_ret);
			else
				muic->status_fault = true;
			muic->have_status = false;
		}
		mutex_unlock(&muic->lock);
		return;
	}

	/* Reading the latched interrupt bytes prevents a permanently asserted pin. */
	regmap_bulk_read(muic->regmap, MAX77849_MUIC_REG_INT1,
			 interrupts, ARRAY_SIZE(interrupts));

	mutex_lock(&muic->lock);
	muic->status_fault = false;
	if (!muic->have_status || status[0] != muic->last_status1 ||
	    status[1] != muic->last_status2) {
		ret = max77849_muic_apply_status(muic, status[0], status[1]);
		if (!ret) {
			muic->last_status1 = status[0];
			muic->last_status2 = status[1];
			muic->have_status = true;
		} else if (ret != -EAGAIN) {
			dev_err_ratelimited(muic->dev,
					    "failed to apply MUIC state: %d\n", ret);
		}
	}
	mutex_unlock(&muic->lock);
}

static void max77849_muic_detect_work(struct work_struct *work)
{
	struct max77849_muic *muic =
		container_of(work, struct max77849_muic, detect_work.work);

	max77849_muic_detect(muic);
	mod_delayed_work(system_wq, &muic->detect_work,
			 msecs_to_jiffies(muic->poll_interval_ms));
}

static irqreturn_t max77849_muic_irq_thread(int irq, void *data)
{
	struct max77849_muic *muic = data;
	u8 interrupts[3];
	int ret;

	ret = regmap_bulk_read(muic->regmap, MAX77849_MUIC_REG_INT1,
			       interrupts, ARRAY_SIZE(interrupts));
	if (ret) {
		dev_err_ratelimited(muic->dev,
				    "failed to acknowledge MUIC interrupt: %d\n",
				    ret);
	}

	mod_delayed_work(system_wq, &muic->detect_work, 0);
	return IRQ_HANDLED;
}

static int max77849_muic_irq_init(struct max77849_muic *muic)
{
	u8 interrupts[3];
	unsigned int ignored;
	int ret;

	if (muic->irq <= 0)
		return 0;

	ret = regmap_update_bits(muic->max77849->regmap,
				 MAX77849_REG_INTSRC_MASK,
				 MAX77849_INTSRC_CHG | MAX77849_INTSRC_MUIC |
				 MAX77849_INTSRC_TOP,
				 MAX77849_INTSRC_CHG | MAX77849_INTSRC_MUIC |
				 MAX77849_INTSRC_TOP);
	if (ret)
		return ret;

	/* Only MUIC events share the physical IRQ until charger IRQs are used. */
	ret = regmap_write(muic->max77849->regmap,
			   MAX77849_REG_CHG_INT_MASK, 0xff);
	if (ret)
		return ret;

	ret = regmap_write(muic->regmap, MAX77849_MUIC_REG_INTMASK1,
			   MAX77849_MUIC_INTMASK1_ADC |
			   MAX77849_MUIC_INTMASK1_ADC1K);
	if (ret)
		return ret;

	ret = regmap_write(muic->regmap, MAX77849_MUIC_REG_INTMASK2,
			   MAX77849_MUIC_INTMASK2_CHGTYP |
			   MAX77849_MUIC_INTMASK2_VBVOLT);
	if (ret)
		return ret;

	ret = regmap_write(muic->regmap, MAX77849_MUIC_REG_INTMASK3, 0);
	if (ret)
		return ret;

	/* Clear bootloader latches before enabling the falling-edge IRQ. */
	ret = regmap_read(muic->max77849->regmap, MAX77849_REG_CHG_INT,
			  &ignored);
	if (ret)
		return ret;

	ret = regmap_bulk_read(muic->regmap, MAX77849_MUIC_REG_INT1,
			       interrupts, ARRAY_SIZE(interrupts));
	if (ret)
		return ret;

	ret = devm_request_threaded_irq(muic->dev, muic->irq, NULL,
					max77849_muic_irq_thread,
					IRQF_ONESHOT, "max77849-muic", muic);
	if (ret)
		return ret;

	ret = regmap_update_bits(muic->max77849->regmap,
				 MAX77849_REG_INTSRC_MASK,
				 MAX77849_INTSRC_MUIC, 0);
	if (ret)
		return ret;

	device_init_wakeup(muic->dev, true);
	return 0;
}

static int max77849_muic_probe(struct platform_device *pdev)
{
	struct max77849_muic *muic;
	unsigned int id;
	int ret;

	muic = devm_kzalloc(&pdev->dev, sizeof(*muic), GFP_KERNEL);
	if (!muic)
		return -ENOMEM;

	muic->dev = &pdev->dev;
	muic->max77849 = dev_get_drvdata(pdev->dev.parent);
	if (!muic->max77849)
		return dev_err_probe(&pdev->dev, -ENODEV,
				     "parent device data unavailable\n");
	muic->regmap = muic->max77849->regmap_muic;
	muic->irq = muic->max77849->irq;
	if (!muic->regmap)
		return dev_err_probe(&pdev->dev, -ENODEV,
				     "MUIC regmap unavailable\n");

	muic->charger = devm_power_supply_get_by_phandle(&pdev->dev,
							 "maxim,charger");
	if (IS_ERR(muic->charger))
		return dev_err_probe(&pdev->dev, PTR_ERR(muic->charger),
				     "charger power supply unavailable\n");
	if (!muic->charger)
		return dev_err_probe(&pdev->dev, -EPROBE_DEFER,
				     "charger power supply not registered yet\n");

	muic->edev = devm_extcon_dev_allocate(&pdev->dev,
					      max77849_muic_cables);
	if (IS_ERR(muic->edev))
		return PTR_ERR(muic->edev);

	ret = devm_extcon_dev_register(&pdev->dev, muic->edev);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to register extcon\n");

	mutex_init(&muic->lock);
	platform_set_drvdata(pdev, muic);

	muic->poll_interval_ms = MAX77849_MUIC_DEFAULT_POLL_INTERVAL_MS;
	device_property_read_u32(&pdev->dev, "maxim,poll-interval-ms",
				 &muic->poll_interval_ms);
	if (muic->poll_interval_ms < 250 || muic->poll_interval_ms > 60000)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "invalid poll interval\n");

	ret = regmap_read(muic->regmap, MAX77849_MUIC_REG_ID, &id);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "failed to read MUIC ID\n");

	ret = regmap_update_bits(muic->regmap, MAX77849_MUIC_REG_CDETCTRL1,
				 MAX77849_MUIC_CDETCTRL1_CHGDETEN,
				 MAX77849_MUIC_CDETCTRL1_CHGDETEN);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to enable charger detection\n");

	ret = devm_delayed_work_autocancel(&pdev->dev, &muic->detect_work,
					   max77849_muic_detect_work);
	if (ret)
		return ret;

	/*
	 * Publish the initial state before completing the supplier probe.  USB
	 * consumers can otherwise read the empty extcon state and miss the
	 * asynchronous first notification while their notifier is not yet
	 * registered.
	 */
	max77849_muic_detect(muic);

	ret = max77849_muic_irq_init(muic);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to initialize MUIC interrupt\n");

	mod_delayed_work(system_wq, &muic->detect_work,
			 msecs_to_jiffies(muic->poll_interval_ms));
	dev_info(&pdev->dev, "MAX77849 MUIC detected (ID=0x%02x)\n", id);

	return 0;
}

static void max77849_muic_disable(struct max77849_muic *muic)
{
	int ret;

	if (muic->irq > 0) {
		ret = regmap_update_bits(muic->max77849->regmap,
					 MAX77849_REG_INTSRC_MASK,
					 MAX77849_INTSRC_MUIC,
					 MAX77849_INTSRC_MUIC);
		if (ret)
			dev_warn(muic->dev, "failed to mask MUIC interrupt: %d\n",
				 ret);
	}

	cancel_delayed_work_sync(&muic->detect_work);

	mutex_lock(&muic->lock);
	ret = max77849_muic_set_psy_property(muic,
					     POWER_SUPPLY_PROP_ONLINE, false);
	if (ret && ret != -ENODEV && ret != -EAGAIN)
		dev_warn(muic->dev, "failed to mark charger offline: %d\n", ret);

	ret = max77849_set_otg(muic->max77849, false);
	if (ret)
		dev_warn(muic->dev, "failed to disable OTG: %d\n", ret);

	ret = max77849_muic_set_path(muic, false, false);
	if (ret)
		dev_warn(muic->dev, "failed to open USB path: %d\n", ret);

	ret = max77849_muic_publish_state(muic, 0);
	if (ret)
		dev_warn(muic->dev, "failed to clear extcon state: %d\n", ret);
	mutex_unlock(&muic->lock);
}

static void max77849_muic_remove(struct platform_device *pdev)
{
	max77849_muic_disable(platform_get_drvdata(pdev));
}

static void max77849_muic_shutdown(struct platform_device *pdev)
{
	struct max77849_muic *muic = platform_get_drvdata(pdev);
	int ret;

	if (muic->irq > 0) {
		ret = regmap_update_bits(muic->max77849->regmap,
					 MAX77849_REG_INTSRC_MASK,
					 MAX77849_INTSRC_MUIC,
					 MAX77849_INTSRC_MUIC);
		if (ret)
			dev_warn(muic->dev,
				 "failed to mask MUIC interrupt at shutdown: %d\n",
				 ret);
	}

	cancel_delayed_work_sync(&muic->detect_work);

	mutex_lock(&muic->lock);
	/*
	 * Never leave VBUS sourcing enabled after shutdown.  Do not mark an
	 * attached input source offline, though: the charger is autonomous and
	 * may safely continue charging with its programmed CC/CV and timers.
	 */
	ret = max77849_set_otg(muic->max77849, false);
	if (ret)
		dev_warn(muic->dev, "failed to disable OTG at shutdown: %d\n",
			 ret);

	ret = max77849_muic_set_path(muic, false, false);
	if (ret)
		dev_warn(muic->dev,
			 "failed to open USB path at shutdown: %d\n", ret);
	mutex_unlock(&muic->lock);
}

static int max77849_muic_suspend(struct device *dev)
{
	struct max77849_muic *muic = dev_get_drvdata(dev);
	int ret;

	cancel_delayed_work_sync(&muic->detect_work);

	mutex_lock(&muic->lock);
	muic->have_status = false;
	muic->status_fault = false;
	mutex_unlock(&muic->lock);

	if (muic->irq > 0 && device_may_wakeup(dev)) {
		ret = enable_irq_wake(muic->irq);
		if (ret) {
			mod_delayed_work(system_wq, &muic->detect_work, 0);
			return ret;
		}
	}

	return 0;
}

static int max77849_muic_resume(struct device *dev)
{
	struct max77849_muic *muic = dev_get_drvdata(dev);

	if (muic->irq > 0 && device_may_wakeup(dev))
		disable_irq_wake(muic->irq);

	mod_delayed_work(system_wq, &muic->detect_work, 0);
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(max77849_muic_pm_ops,
				max77849_muic_suspend, max77849_muic_resume);

static const struct of_device_id max77849_muic_of_match[] = {
	{ .compatible = "maxim,max77849-muic" },
	{ }
};
MODULE_DEVICE_TABLE(of, max77849_muic_of_match);

static struct platform_driver max77849_muic_driver = {
	.driver = {
		.name = "max77849-muic",
		.of_match_table = max77849_muic_of_match,
		.pm = pm_sleep_ptr(&max77849_muic_pm_ops),
	},
	.probe = max77849_muic_probe,
	.remove_new = max77849_muic_remove,
	.shutdown = max77849_muic_shutdown,
};
module_platform_driver(max77849_muic_driver);

MODULE_AUTHOR("Sean Hoyt <seanhoyt963@gmail.com>");
MODULE_DESCRIPTION("Maxim MAX77849 MUIC and USB OTG driver");
MODULE_LICENSE("GPL");
