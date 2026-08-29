// SPDX-License-Identifier: GPL-2.0-only

#include <linux/delay.h>
#include <linux/devm-helpers.h>
#include <linux/extcon.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/input/touchscreen.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

/* Register Map */

#define ZINITIX_SWRESET_CMD			0x0000
#define ZINITIX_WAKEUP_CMD			0x0001

#define ZINITIX_IDLE_CMD			0x0004
#define ZINITIX_SLEEP_CMD			0x0005

#define ZINITIX_CLEAR_INT_STATUS_CMD		0x0003
#define ZINITIX_CALIBRATE_CMD			0x0006
#define ZINITIX_SAVE_STATUS_CMD			0x0007
#define ZINITIX_SAVE_CALIBRATION_CMD		0x0008
#define ZINITIX_RECALL_FACTORY_CMD		0x000f

#define ZINITIX_THRESHOLD			0x0020

#define ZINITIX_LARGE_PALM_REJECT_AREA_TH	0x003F

#define ZINITIX_DEBUG_REG			0x0115 /* 0~7 */

#define ZINITIX_TOUCH_MODE			0x0010

#define ZINITIX_CHIP_REVISION			0x0011
#define ZINITIX_CHIP_BTX0X_MASK			0xF0F0
#define ZINITIX_CHIP_BT4X2			0x4020
#define ZINITIX_CHIP_BT4X3			0x4030
#define ZINITIX_CHIP_BT4X4			0x4040

#define ZINITIX_FIRMWARE_VERSION		0x0012

#define ZINITIX_USB_DETECT			0x116

#define ZINITIX_MINOR_FW_VERSION		0x0121

#define ZINITIX_VENDOR_ID			0x001C
#define ZINITIX_HW_ID				0x0014

#define ZINITIX_DATA_VERSION_REG		0x0013
#define ZINITIX_SUPPORTED_FINGER_NUM		0x0015
#define ZINITIX_EEPROM_INFO			0x0018
#define ZINITIX_INITIAL_TOUCH_MODE		0x0019

#define ZINITIX_TOTAL_NUMBER_OF_X		0x0060
#define ZINITIX_TOTAL_NUMBER_OF_Y		0x0061

#define ZINITIX_DELAY_RAW_FOR_HOST		0x007f

#define ZINITIX_BUTTON_SUPPORTED_NUM		0x00B0
#define ZINITIX_BUTTON_SENSITIVITY		0x00B2
#define ZINITIX_DUMMY_BUTTON_SENSITIVITY	0X00C8

#define ZINITIX_X_RESOLUTION			0x00C0
#define ZINITIX_Y_RESOLUTION			0x00C1

#define ZINITIX_POINT_STATUS_REG		0x0080

#define ZINITIX_BT4X2_ICON_STATUS_REG		0x009A
#define ZINITIX_BT4X3_ICON_STATUS_REG		0x00A0
#define ZINITIX_BT4X4_ICON_STATUS_REG		0x00A0
#define ZINITIX_BT5XX_ICON_STATUS_REG		0x00AA

#define ZINITIX_POINT_COORD_REG			(ZINITIX_POINT_STATUS_REG + 2)

#define ZINITIX_AFE_FREQUENCY			0x0100
#define ZINITIX_DND_N_COUNT			0x0122
#define ZINITIX_DND_U_COUNT			0x0135

#define ZINITIX_RAWDATA_REG			0x0200

#define ZINITIX_EEPROM_INFO_REG			0x0018

#define ZINITIX_INT_ENABLE_FLAG			0x00f0
#define ZINITIX_PERIODICAL_INTERRUPT_INTERVAL	0x00f1

#define ZINITIX_BTN_WIDTH			0x016d

#define ZINITIX_CHECKSUM_RESULT			0x012c

#define ZINITIX_INIT_FLASH			0x01d0
#define ZINITIX_WRITE_FLASH			0x01d1
#define ZINITIX_READ_FLASH			0x01d2

#define ZINITIX_INTERNAL_FLAG_02		0x011e
#define ZINITIX_INTERNAL_FLAG_03		0x011f

#define ZINITIX_I2C_CHECKSUM_WCNT		0x016a
#define ZINITIX_I2C_CHECKSUM_RESULT		0x016c

/* Interrupt & status register flags */

#define BIT_PT_CNT_CHANGE			BIT(0)
#define BIT_DOWN				BIT(1)
#define BIT_MOVE				BIT(2)
#define BIT_UP					BIT(3)
#define BIT_PALM				BIT(4)
#define BIT_PALM_REJECT				BIT(5)
#define BIT_RESERVED_0				BIT(6)
#define BIT_RESERVED_1				BIT(7)
#define BIT_WEIGHT_CHANGE			BIT(8)
#define BIT_PT_NO_CHANGE			BIT(9)
#define BIT_REJECT				BIT(10)
#define BIT_PT_EXIST				BIT(11)
#define BIT_RESERVED_2				BIT(12)
#define BIT_ERROR				BIT(13)
#define BIT_DEBUG				BIT(14)
#define BIT_ICON_EVENT				BIT(15)

#define SUB_BIT_EXIST				BIT(0)
#define SUB_BIT_DOWN				BIT(1)
#define SUB_BIT_MOVE				BIT(2)
#define SUB_BIT_UP				BIT(3)
#define SUB_BIT_UPDATE				BIT(4)
#define SUB_BIT_WAIT				BIT(5)

#define DEFAULT_TOUCH_POINT_MODE		2
#define MAX_SUPPORTED_FINGER_NUM		5
#define MAX_SUPPORTED_BUTTON_NUM		8

#define CHIP_ON_DELAY				15 // ms
#define FIRMWARE_ON_DELAY			40 // ms

struct point_coord {
	__le16	x;
	__le16	y;
	u8	width;
	u8	sub_status;
};

struct point_coord_mode2 {
	struct point_coord coord;
	// currently unused, but needed as padding:
	u8	minor_width;
	u8	angle;
};

struct touch_event_mode1 {
	__le16	status;
	__le16	event_flag;
	struct point_coord point_coord[MAX_SUPPORTED_FINGER_NUM];
};

struct touch_event_mode2 {
	__le16	status;
	u8	finger_mask;
	u8	time_stamp;
	struct point_coord_mode2 point_coord[MAX_SUPPORTED_FINGER_NUM];
};

struct zinitix_chip_info {
	u8 default_touch_mode;
	u16 power_on_delay_ms;
	u16 firmware_on_delay_ms;
	u16 read_delay_us;
	u16 post_transaction_delay_us;
	u16 firmware_checksum;
	bool has_usb_detect;
	bool needs_second_reset;
};

struct bt541_ts_data {
	struct i2c_client *client;
	struct input_dev *input_dev;
	struct touchscreen_properties prop;
	struct regulator_bulk_data supplies[2];
	struct extcon_dev *extcon;
	struct notifier_block extcon_nb;
	struct work_struct extcon_work;
	const struct zinitix_chip_info *chip_info;
	u32 zinitix_mode;
	u32 keycodes[MAX_SUPPORTED_BUTTON_NUM];
	int num_keycodes;
	bool have_versioninfo;
	u16 chip_revision;
	u16 firmware_version;
	u16 regdata_version;
	u16 icon_status_reg;
};

static void zinitix_read_delay(struct i2c_client *client)
{
	struct bt541_ts_data *bt541 = i2c_get_clientdata(client);

	if (bt541->chip_info)
		udelay(bt541->chip_info->read_delay_us);
}

static void zinitix_post_transaction_delay(struct i2c_client *client)
{
	struct bt541_ts_data *bt541 = i2c_get_clientdata(client);

	if (bt541->chip_info)
		udelay(bt541->chip_info->post_transaction_delay_us);
}

static int zinitix_read_data(struct i2c_client *client,
			     u16 reg, void *values, size_t length)
{
	__le16 reg_le = cpu_to_le16(reg);
	int ret;

	/* A single i2c_transfer() transaction does not work here. */
	ret = i2c_master_send(client, (u8 *)&reg_le, sizeof(reg_le));
	if (ret != sizeof(reg_le))
		return ret < 0 ? ret : -EIO;
	zinitix_read_delay(client);

	ret = i2c_master_recv(client, (u8 *)values, length);
	if (ret != length)
		return ret < 0 ? ret : -EIO;
	zinitix_post_transaction_delay(client);

	return 0;
}

static int zinitix_recv_data(struct i2c_client *client, void *values,
			     size_t length)
{
	int ret;

	ret = i2c_master_recv(client, values, length);
	if (ret != length)
		return ret < 0 ? ret : -EIO;
	zinitix_post_transaction_delay(client);

	return 0;
}

static int zinitix_write_u16(struct i2c_client *client, u16 reg, u16 value)
{
	__le16 packet[2] = {cpu_to_le16(reg), cpu_to_le16(value)};
	int ret;

	ret = i2c_master_send(client, (u8 *)packet, sizeof(packet));
	if (ret != sizeof(packet))
		return ret < 0 ? ret : -EIO;
	zinitix_post_transaction_delay(client);

	return 0;
}

static int zinitix_write_cmd(struct i2c_client *client, u16 reg)
{
	__le16 reg_le = cpu_to_le16(reg);
	int ret;

	ret = i2c_master_send(client, (u8 *)&reg_le, sizeof(reg_le));
	if (ret != sizeof(reg_le))
		return ret < 0 ? ret : -EIO;
	zinitix_post_transaction_delay(client);

	return 0;
}

static int zinitix_set_usb_detect(struct bt541_ts_data *bt541)
{
	static const unsigned int charger_cables[] = {
		EXTCON_CHG_USB_SDP,
		EXTCON_CHG_USB_CDP,
		EXTCON_CHG_USB_DCP,
	};
	struct i2c_client *client = bt541->client;
	unsigned int value;
	__le16 raw;
	bool connected = false;
	int error;
	int i;

	for (i = 0; i < ARRAY_SIZE(charger_cables); i++) {
		error = extcon_get_state(bt541->extcon, charger_cables[i]);
		if (error < 0)
			return error;
		if (error > 0) {
			connected = true;
			break;
		}
	}

	error = zinitix_read_data(client, ZINITIX_USB_DETECT, &raw, sizeof(raw));
	if (error)
		return error;

	value = le16_to_cpu(raw);
	if (!!(value & BIT(0)) == connected)
		return 0;

	if (connected)
		value |= BIT(0);
	else
		value &= ~BIT(0);

	error = zinitix_write_u16(client, ZINITIX_USB_DETECT, value);
	if (!error)
		dev_dbg(&client->dev, "USB charger mode %s\n",
			connected ? "enabled" : "disabled");

	return error;
}

static void zinitix_extcon_work(struct work_struct *work)
{
	struct bt541_ts_data *bt541 = container_of(work, struct bt541_ts_data,
						   extcon_work);
	int error = 0;

	mutex_lock(&bt541->input_dev->mutex);
	if (input_device_enabled(bt541->input_dev))
		error = zinitix_set_usb_detect(bt541);
	mutex_unlock(&bt541->input_dev->mutex);

	if (error)
		dev_warn_ratelimited(&bt541->client->dev,
				     "Failed to update USB charger mode: %d\n",
				     error);
}

static int zinitix_extcon_notifier(struct notifier_block *nb,
				   unsigned long event, void *ptr)
{
	struct bt541_ts_data *bt541 = container_of(nb, struct bt541_ts_data,
						   extcon_nb);

	schedule_work(&bt541->extcon_work);
	return NOTIFY_OK;
}

static int zinitix_read_u16(struct bt541_ts_data *bt541, u16 reg, u16 *value)
{
	__le16 raw;
	int error;

	error = zinitix_read_data(bt541->client, reg, &raw, sizeof(raw));
	if (error)
		return error;

	*value = le16_to_cpu(raw);
	return 0;
}

static int zinitix_init_touch(struct bt541_ts_data *bt541)
{
	struct i2c_client *client = bt541->client;
	int i;
	int error;
	u16 int_flags;

	error = zinitix_write_cmd(client, ZINITIX_SWRESET_CMD);
	if (error) {
		dev_err(&client->dev, "Failed to write reset command\n");
		return error;
	}

	if (bt541->chip_info && bt541->chip_info->needs_second_reset) {
		error = zinitix_write_u16(client, ZINITIX_INT_ENABLE_FLAG, 0);
		if (error)
			return error;

		error = zinitix_write_cmd(client, ZINITIX_SWRESET_CMD);
		if (error)
			return error;
	}

	/*
	 * Read and cache the chip revision and firmware version the first time
	 * we get here.
	 */
	if (!bt541->have_versioninfo) {
		error = zinitix_read_u16(bt541, ZINITIX_CHIP_REVISION,
					 &bt541->chip_revision);
		if (error)
			return error;

		error = zinitix_read_u16(bt541, ZINITIX_FIRMWARE_VERSION,
					 &bt541->firmware_version);
		if (error)
			return error;

		error = zinitix_read_u16(bt541, ZINITIX_DATA_VERSION_REG,
					 &bt541->regdata_version);
		if (error)
			return error;

		bt541->have_versioninfo = true;

		dev_dbg(&client->dev,
			"chip revision %04x firmware version %04x regdata version %04x\n",
			bt541->chip_revision, bt541->firmware_version,
			bt541->regdata_version);

		/*
		 * Determine the "icon" status register which varies by the
		 * chip.
		 */
		switch (bt541->chip_revision & ZINITIX_CHIP_BTX0X_MASK) {
		case ZINITIX_CHIP_BT4X2:
			bt541->icon_status_reg = ZINITIX_BT4X2_ICON_STATUS_REG;
			break;

		case ZINITIX_CHIP_BT4X3:
			bt541->icon_status_reg = ZINITIX_BT4X3_ICON_STATUS_REG;
			break;

		case ZINITIX_CHIP_BT4X4:
			bt541->icon_status_reg = ZINITIX_BT4X4_ICON_STATUS_REG;
			break;

		default:
			bt541->icon_status_reg = ZINITIX_BT5XX_ICON_STATUS_REG;
			break;
		}
	}

	error = zinitix_write_u16(client, ZINITIX_INT_ENABLE_FLAG, 0x0);
	if (error) {
		dev_err(&client->dev,
			"Failed to reset interrupt enable flag\n");
		return error;
	}

	/* initialize */
	error = zinitix_write_u16(client, ZINITIX_X_RESOLUTION,
				  bt541->prop.max_x);
	if (error)
		return error;

	error = zinitix_write_u16(client, ZINITIX_Y_RESOLUTION,
				  bt541->prop.max_y);
	if (error)
		return error;

	error = zinitix_write_u16(client, ZINITIX_SUPPORTED_FINGER_NUM,
				  MAX_SUPPORTED_FINGER_NUM);
	if (error)
		return error;

	error = zinitix_write_u16(client, ZINITIX_BUTTON_SUPPORTED_NUM,
				  bt541->num_keycodes);
	if (error)
		return error;

	error = zinitix_write_u16(client, ZINITIX_INITIAL_TOUCH_MODE,
				  bt541->zinitix_mode);
	if (error)
		return error;

	error = zinitix_write_u16(client, ZINITIX_TOUCH_MODE,
				  bt541->zinitix_mode);
	if (error)
		return error;

	int_flags = BIT_PT_CNT_CHANGE | BIT_DOWN | BIT_MOVE | BIT_UP;
	if (bt541->num_keycodes)
		int_flags |= BIT_ICON_EVENT;

	error = zinitix_write_u16(client, ZINITIX_INT_ENABLE_FLAG, int_flags);
	if (error)
		return error;

	/* clear queue */
	for (i = 0; i < 10; i++) {
		zinitix_write_cmd(client, ZINITIX_CLEAR_INT_STATUS_CMD);
		udelay(10);
	}

	return 0;
}

static int zinitix_init_regulators(struct bt541_ts_data *bt541)
{
	struct device *dev = &bt541->client->dev;
	int error;

	/*
	 * Some older device trees have erroneous names for the regulators,
	 * so check if "vddo" is present and in that case use these names.
	 * Else use the proper supply names on the component.
	 */
	if (of_property_present(dev->of_node, "vddo-supply")) {
		bt541->supplies[0].supply = "vdd";
		bt541->supplies[1].supply = "vddo";
	} else {
		/* Else use the proper supply names */
		bt541->supplies[0].supply = "vcca";
		bt541->supplies[1].supply = "vdd";
	}
	error = devm_regulator_bulk_get(dev,
					ARRAY_SIZE(bt541->supplies),
					bt541->supplies);
	if (error < 0) {
		dev_err(dev, "Failed to get regulators: %d\n", error);
		return error;
	}

	return 0;
}

static int zinitix_send_power_on_sequence(struct bt541_ts_data *bt541)
{
	unsigned int firmware_on_delay_ms = FIRMWARE_ON_DELAY;
	int error;
	struct i2c_client *client = bt541->client;

	if (bt541->chip_info && bt541->chip_info->firmware_on_delay_ms)
		firmware_on_delay_ms = bt541->chip_info->firmware_on_delay_ms;

	error = zinitix_write_u16(client, 0xc000, 0x0001);
	if (error) {
		dev_err(&client->dev,
			"Failed to send power sequence(vendor cmd enable)\n");
		return error;
	}
	udelay(10);

	error = zinitix_write_cmd(client, 0xc004);
	if (error) {
		dev_err(&client->dev,
			"Failed to send power sequence (intn clear)\n");
		return error;
	}
	udelay(10);

	error = zinitix_write_u16(client, 0xc002, 0x0001);
	if (error) {
		dev_err(&client->dev,
			"Failed to send power sequence (nvm init)\n");
		return error;
	}
	mdelay(2);

	error = zinitix_write_u16(client, 0xc001, 0x0001);
	if (error) {
		dev_err(&client->dev,
			"Failed to send power sequence (program start)\n");
		return error;
	}
	msleep(firmware_on_delay_ms);

	return 0;
}

static int zinitix_check_firmware(struct bt541_ts_data *bt541)
{
	u16 checksum;
	int error;

	if (!bt541->chip_info || !bt541->chip_info->firmware_checksum)
		return 0;

	error = zinitix_read_u16(bt541, ZINITIX_CHECKSUM_RESULT, &checksum);
	if (error)
		return error;

	if (checksum != bt541->chip_info->firmware_checksum) {
		dev_err(&bt541->client->dev,
			"Firmware checksum mismatch: got %#06x, expected %#06x\n",
			checksum, bt541->chip_info->firmware_checksum);
		return -EILSEQ;
	}

	dev_dbg(&bt541->client->dev, "firmware checksum %#06x\n", checksum);
	return 0;
}

static void zinitix_report_finger(struct bt541_ts_data *bt541, int slot,
				  const struct point_coord *p, bool palm)
{
	bool active;
	u16 x, y;
	u8 width;

	if (unlikely(!(p->sub_status & (SUB_BIT_EXIST | SUB_BIT_UP)))) {
		dev_dbg(&bt541->client->dev, "unknown finger event %#02x\n",
			p->sub_status);
		return;
	}

	x = le16_to_cpu(p->x);
	y = le16_to_cpu(p->y);
	width = max_t(u8, p->width, 1);
	active = (p->sub_status & SUB_BIT_EXIST) &&
		 !(p->sub_status & SUB_BIT_UP);

	input_mt_slot(bt541->input_dev, slot);
	if (input_mt_report_slot_state(bt541->input_dev,
				       palm ? MT_TOOL_PALM : MT_TOOL_FINGER,
				       active)) {
		touchscreen_report_pos(bt541->input_dev,
				       &bt541->prop, x, y, true);
		input_report_abs(bt541->input_dev,
				 ABS_MT_TOUCH_MAJOR, width);
		dev_dbg(&bt541->client->dev, "finger %d %s (%u, %u)\n",
			slot, p->sub_status & SUB_BIT_DOWN ? "down" :
			p->sub_status & SUB_BIT_MOVE ? "move" : "active",
			x, y);
	} else {
		dev_dbg(&bt541->client->dev, "finger %d up (%u, %u)\n",
			slot, x, y);
	}
}

static void zinitix_release_inputs(struct bt541_ts_data *bt541)
{
	int i;

	for (i = 0; i < MAX_SUPPORTED_FINGER_NUM; i++) {
		input_mt_slot(bt541->input_dev, i);
		input_mt_report_slot_state(bt541->input_dev, MT_TOOL_FINGER, false);
	}
	for (i = 0; i < bt541->num_keycodes; i++)
		input_report_key(bt541->input_dev, bt541->keycodes[i], 0);

	input_mt_sync_frame(bt541->input_dev);
	input_sync(bt541->input_dev);
}

static int zinitix_read_mode1_event(struct bt541_ts_data *bt541,
				    struct touch_event_mode1 *touch_event)
{
	struct i2c_client *client = bt541->client;
	size_t initial_length = offsetof(struct touch_event_mode1,
					 point_coord[1]);
	unsigned long event_mask;
	int error;
	int i;

	/*
	 * In mode 1 the controller places the first event at its I2C read
	 * pointer when it asserts IRQ. The downstream ZT7554 driver reads this
	 * initial status/event/coordinate block without selecting a register,
	 * then fetches the remaining changed coordinates individually.
	 */
	error = zinitix_recv_data(client, touch_event, initial_length);
	if (error)
		return error;

	event_mask = le16_to_cpu(touch_event->event_flag);
	for_each_set_bit(i, &event_mask, MAX_SUPPORTED_FINGER_NUM) {
		if (i == 0)
			continue;

		udelay(20);
		error = zinitix_read_data(client,
					  ZINITIX_POINT_STATUS_REG + 2 + i * 4,
					  &touch_event->point_coord[i],
					  sizeof(touch_event->point_coord[i]));
		if (error)
			return error;
	}

	return 0;
}

static void zinitix_report_keys(struct bt541_ts_data *bt541, u16 icon_events)
{
	int i;

	for (i = 0; i < bt541->num_keycodes; i++)
		input_report_key(bt541->input_dev,
				 bt541->keycodes[i], icon_events & BIT(i));
}

static irqreturn_t zinitix_ts_irq_handler(int irq, void *bt541_handler)
{
	struct bt541_ts_data *bt541 = bt541_handler;
	struct i2c_client *client = bt541->client;
	struct touch_event_mode1 touch_event_mode1 = { };
	struct touch_event_mode2 touch_event_mode2 = { };
	const struct point_coord *point_coord;
	unsigned long finger_mask;
	__le16 icon_events;
	u16 status;
	int error;
	int i;

	if (bt541->zinitix_mode == 1) {
		error = zinitix_read_mode1_event(bt541, &touch_event_mode1);
		status = le16_to_cpu(touch_event_mode1.status);
		finger_mask = le16_to_cpu(touch_event_mode1.event_flag);
	} else {
		error = zinitix_read_data(client, ZINITIX_POINT_STATUS_REG,
					  &touch_event_mode2,
					  sizeof(touch_event_mode2));
		status = le16_to_cpu(touch_event_mode2.status);
		finger_mask = touch_event_mode2.finger_mask;
	}
	if (error) {
		dev_err(&client->dev, "Failed to read in touchpoint struct\n");
		goto out;
	}

	if (status & BIT_ICON_EVENT) {
		error = zinitix_read_data(bt541->client, bt541->icon_status_reg,
					  &icon_events, sizeof(icon_events));
		if (error) {
			dev_err(&client->dev, "Failed to read icon events\n");
			goto out;
		}

		zinitix_report_keys(bt541, le16_to_cpu(icon_events));
	}

	/* Mode 1 also generates interrupts without a coordinate event. */
	if (bt541->zinitix_mode == 1 && (!status || !finger_mask)) {
		if (status & BIT_ICON_EVENT)
			input_sync(bt541->input_dev);
		goto out;
	}

	for_each_set_bit(i, &finger_mask, MAX_SUPPORTED_FINGER_NUM) {
		if (bt541->zinitix_mode == 1)
			point_coord = &touch_event_mode1.point_coord[i];
		else
			point_coord = &touch_event_mode2.point_coord[i].coord;

		/* Explicit UP events need reporting even without SUB_BIT_EXIST. */
		if (point_coord->sub_status & (SUB_BIT_EXIST | SUB_BIT_UP))
			zinitix_report_finger(bt541, i, point_coord,
					      status & (BIT_PALM | BIT_PALM_REJECT));
	}

	input_mt_sync_frame(bt541->input_dev);
	input_sync(bt541->input_dev);

out:
	zinitix_write_cmd(bt541->client, ZINITIX_CLEAR_INT_STATUS_CMD);
	return IRQ_HANDLED;
}

static int zinitix_start(struct bt541_ts_data *bt541)
{
	unsigned int power_on_delay_ms = CHIP_ON_DELAY;
	int error;

	if (bt541->chip_info && bt541->chip_info->power_on_delay_ms)
		power_on_delay_ms = bt541->chip_info->power_on_delay_ms;

	error = regulator_bulk_enable(ARRAY_SIZE(bt541->supplies),
				      bt541->supplies);
	if (error) {
		dev_err(&bt541->client->dev,
			"Failed to enable regulators: %d\n", error);
		return error;
	}

	msleep(power_on_delay_ms);

	error = zinitix_send_power_on_sequence(bt541);
	if (error) {
		dev_err(&bt541->client->dev,
			"Error while sending power-on sequence: %d\n", error);
		goto disable_regulators;
	}

	error = zinitix_check_firmware(bt541);
	if (error) {
		dev_err(&bt541->client->dev,
			"Failed to validate touch firmware: %d\n", error);
		goto disable_regulators;
	}

	error = zinitix_init_touch(bt541);
	if (error) {
		dev_err(&bt541->client->dev,
			"Error while configuring touch IC\n");
		goto disable_regulators;
	}

	if (bt541->extcon) {
		error = zinitix_set_usb_detect(bt541);
		if (error)
			dev_warn(&bt541->client->dev,
				 "Failed to set USB charger mode: %d\n", error);
	}

	enable_irq(bt541->client->irq);

	return 0;

disable_regulators:
	regulator_bulk_disable(ARRAY_SIZE(bt541->supplies), bt541->supplies);
	return error;
}

static int zinitix_stop(struct bt541_ts_data *bt541)
{
	int error;

	disable_irq(bt541->client->irq);
	zinitix_release_inputs(bt541);

	error = zinitix_write_cmd(bt541->client, ZINITIX_SLEEP_CMD);
	if (error)
		dev_dbg(&bt541->client->dev,
			"Failed to send sleep command: %d\n", error);

	error = regulator_bulk_disable(ARRAY_SIZE(bt541->supplies),
				       bt541->supplies);
	if (error) {
		dev_err(&bt541->client->dev,
			"Failed to disable regulators: %d\n", error);
		return error;
	}

	return 0;
}

static int zinitix_input_open(struct input_dev *dev)
{
	struct bt541_ts_data *bt541 = input_get_drvdata(dev);

	return zinitix_start(bt541);
}

static void zinitix_input_close(struct input_dev *dev)
{
	struct bt541_ts_data *bt541 = input_get_drvdata(dev);

	zinitix_stop(bt541);
}

static int zinitix_init_input_dev(struct bt541_ts_data *bt541)
{
	struct input_dev *input_dev;
	int error;
	int i;

	input_dev = devm_input_allocate_device(&bt541->client->dev);
	if (!input_dev) {
		dev_err(&bt541->client->dev,
			"Failed to allocate input device.");
		return -ENOMEM;
	}

	input_set_drvdata(input_dev, bt541);
	bt541->input_dev = input_dev;

	input_dev->name = "Zinitix Capacitive TouchScreen";
	input_dev->phys = "input/ts";
	input_dev->id.bustype = BUS_I2C;
	input_dev->open = zinitix_input_open;
	input_dev->close = zinitix_input_close;

	if (bt541->num_keycodes) {
		input_dev->keycode = bt541->keycodes;
		input_dev->keycodemax = bt541->num_keycodes;
		input_dev->keycodesize = sizeof(bt541->keycodes[0]);
		for (i = 0; i < bt541->num_keycodes; i++)
			input_set_capability(input_dev, EV_KEY, bt541->keycodes[i]);
	}

	input_set_capability(input_dev, EV_ABS, ABS_MT_POSITION_X);
	input_set_capability(input_dev, EV_ABS, ABS_MT_POSITION_Y);
	input_set_abs_params(input_dev, ABS_MT_WIDTH_MAJOR, 0, 255, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_TOOL_TYPE, 0, MT_TOOL_PALM, 0, 0);

	touchscreen_parse_properties(input_dev, true, &bt541->prop);
	if (!bt541->prop.max_x || !bt541->prop.max_y) {
		dev_err(&bt541->client->dev,
			"Touchscreen-size-x and/or touchscreen-size-y not set in dts\n");
		return -EINVAL;
	}

	error = input_mt_init_slots(input_dev, MAX_SUPPORTED_FINGER_NUM,
				    INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED);
	if (error) {
		dev_err(&bt541->client->dev,
			"Failed to initialize MT slots: %d", error);
		return error;
	}

	error = input_register_device(input_dev);
	if (error) {
		dev_err(&bt541->client->dev,
			"Failed to register input device: %d", error);
		return error;
	}

	return 0;
}

static int zinitix_ts_probe(struct i2c_client *client)
{
	struct bt541_ts_data *bt541;
	int error;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		dev_err(&client->dev,
			"Failed to assert adapter's support for plain I2C.\n");
		return -ENXIO;
	}

	bt541 = devm_kzalloc(&client->dev, sizeof(*bt541), GFP_KERNEL);
	if (!bt541)
		return -ENOMEM;

	bt541->client = client;
	bt541->chip_info = i2c_get_match_data(client);
	i2c_set_clientdata(client, bt541);

	error = zinitix_init_regulators(bt541);
	if (error) {
		dev_err(&client->dev,
			"Failed to initialize regulators: %d\n", error);
		return error;
	}

	error = devm_request_threaded_irq(&client->dev, client->irq,
					  NULL, zinitix_ts_irq_handler,
					  IRQF_ONESHOT | IRQF_NO_AUTOEN,
					  client->name, bt541);
	if (error) {
		dev_err(&client->dev, "Failed to request IRQ: %d\n", error);
		return error;
	}

	if (device_property_present(&client->dev, "linux,keycodes")) {
		bt541->num_keycodes = device_property_count_u32(&client->dev,
								"linux,keycodes");
		if (bt541->num_keycodes < 0) {
			dev_err(&client->dev, "Failed to count keys (%d)\n",
				bt541->num_keycodes);
			return bt541->num_keycodes;
		} else if (bt541->num_keycodes > ARRAY_SIZE(bt541->keycodes)) {
			dev_err(&client->dev, "Too many keys defined (%d)\n",
				bt541->num_keycodes);
			return -EINVAL;
		}

		error = device_property_read_u32_array(&client->dev,
						       "linux,keycodes",
						       bt541->keycodes,
						       bt541->num_keycodes);
		if (error) {
			dev_err(&client->dev,
				"Unable to parse \"linux,keycodes\" property: %d\n",
				error);
			return error;
		}
	}

	error = device_property_read_u32(&client->dev, "zinitix,mode",
					 &bt541->zinitix_mode);
	if (error < 0) {
		bt541->zinitix_mode = bt541->chip_info ?
					bt541->chip_info->default_touch_mode :
					DEFAULT_TOUCH_POINT_MODE;
	}

	if (bt541->zinitix_mode != 1 && bt541->zinitix_mode != 2) {
		dev_err(&client->dev,
			"Malformed zinitix,mode property, must be 1 or 2 (supplied: %d)\n",
			bt541->zinitix_mode);
		return -EINVAL;
	}

	if (bt541->chip_info && bt541->chip_info->has_usb_detect &&
	    device_property_present(&client->dev, "extcon")) {
		bt541->extcon = extcon_get_edev_by_phandle(&client->dev, 0);
		if (IS_ERR(bt541->extcon))
			return dev_err_probe(&client->dev, PTR_ERR(bt541->extcon),
					     "Failed to get USB extcon\n");
	}

	error = zinitix_init_input_dev(bt541);
	if (error) {
		dev_err(&client->dev,
			"Failed to initialize input device: %d\n", error);
		return error;
	}

	if (bt541->extcon) {
		error = devm_work_autocancel(&client->dev, &bt541->extcon_work,
					     zinitix_extcon_work);
		if (error)
			return error;

		bt541->extcon_nb.notifier_call = zinitix_extcon_notifier;
		error = devm_extcon_register_notifier_all(&client->dev,
							  bt541->extcon,
							  &bt541->extcon_nb);
		if (error)
			return dev_err_probe(&client->dev, error,
					     "Failed to register USB extcon notifier\n");
	}

	return 0;
}

static int zinitix_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct bt541_ts_data *bt541 = i2c_get_clientdata(client);

	guard(mutex)(&bt541->input_dev->mutex);

	if (input_device_enabled(bt541->input_dev))
		zinitix_stop(bt541);

	return 0;
}

static int zinitix_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct bt541_ts_data *bt541 = i2c_get_clientdata(client);
	int error;

	guard(mutex)(&bt541->input_dev->mutex);

	if (input_device_enabled(bt541->input_dev)) {
		error = zinitix_start(bt541);
		if (error)
			return error;
	}

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(zinitix_pm_ops, zinitix_suspend, zinitix_resume);

#ifdef CONFIG_OF
static const struct zinitix_chip_info zinitix_zt7554_chip_info = {
	.default_touch_mode = 1,
	.power_on_delay_ms = 400,
	.firmware_on_delay_ms = 150,
	.read_delay_us = 50,
	.post_transaction_delay_us = 10,
	.firmware_checksum = 0x55aa,
	.has_usb_detect = true,
	.needs_second_reset = true,
};

static const struct of_device_id zinitix_of_match[] = {
	{ .compatible = "zinitix,bt402" },
	{ .compatible = "zinitix,bt403" },
	{ .compatible = "zinitix,bt404" },
	{ .compatible = "zinitix,bt412" },
	{ .compatible = "zinitix,bt413" },
	{ .compatible = "zinitix,bt431" },
	{ .compatible = "zinitix,bt432" },
	{ .compatible = "zinitix,bt531" },
	{ .compatible = "zinitix,bt532" },
	{ .compatible = "zinitix,bt538" },
	{ .compatible = "zinitix,bt541" },
	{ .compatible = "zinitix,bt548" },
	{ .compatible = "zinitix,bt554" },
	{ .compatible = "zinitix,at100" },
	{
		.compatible = "zinitix,zt7554",
		.data = &zinitix_zt7554_chip_info,
	},
	{ }
};
MODULE_DEVICE_TABLE(of, zinitix_of_match);
#endif

static struct i2c_driver zinitix_ts_driver = {
	.probe = zinitix_ts_probe,
	.driver = {
		.name = "Zinitix-TS",
		.pm = pm_sleep_ptr(&zinitix_pm_ops),
		.of_match_table = of_match_ptr(zinitix_of_match),
	},
};
module_i2c_driver(zinitix_ts_driver);

MODULE_AUTHOR("Michael Srba <Michael.Srba@seznam.cz>");
MODULE_DESCRIPTION("Zinitix touchscreen driver");
MODULE_LICENSE("GPL v2");
