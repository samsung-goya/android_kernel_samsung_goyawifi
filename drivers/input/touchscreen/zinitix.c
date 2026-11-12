// SPDX-License-Identifier: GPL-2.0-only
/*
 * Zinitix BT4xx / BT5xx I2C touchscreen driver
 *
 * Back-ported to Linux 4.19 with BT532 point-mode 0 support for
 * Samsung Galaxy Tab 3 Lite (SM-T110 / goyawifi).
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/input/touchscreen.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>

/* Register Map */
#define ZINITIX_SWRESET_CMD			0x0000
#define ZINITIX_WAKEUP_CMD			0x0001
#define ZINITIX_CLEAR_INT_STATUS_CMD		0x0003
#define ZINITIX_IDLE_CMD			0x0004
#define ZINITIX_SLEEP_CMD			0x0005
#define ZINITIX_CALIBRATE_CMD			0x0006

#define ZINITIX_TOUCH_MODE			0x0010
#define ZINITIX_CHIP_REVISION			0x0011
#define ZINITIX_FIRMWARE_VERSION		0x0012
#define ZINITIX_DATA_VERSION_REG		0x0013
#define ZINITIX_HW_ID				0x0014
#define ZINITIX_SUPPORTED_FINGER_NUM		0x0015
#define ZINITIX_EEPROM_INFO			0x0018
#define ZINITIX_INITIAL_TOUCH_MODE		0x0019

#define ZINITIX_POINT_STATUS_REG		0x0080
#define ZINITIX_ICON_STATUS_REG			0x00aa
#define ZINITIX_BUTTON_SUPPORTED_NUM		0x00b0
#define ZINITIX_X_RESOLUTION			0x00c0
#define ZINITIX_Y_RESOLUTION			0x00c1
#define ZINITIX_INT_ENABLE_FLAG			0x00f0

/* Vendor power-on sequence commands */
#define ZINITIX_VENDOR_CMD_ENABLE		0xc000
#define ZINITIX_PROGRAM_START			0xc001
#define ZINITIX_NVM_INIT			0xc002
#define ZINITIX_INTN_CLEAR			0xc004
#define ZINITIX_CHIP_CODE_REG			0xcc00

/* Interrupt & status register flags */
#define BIT_PT_CNT_CHANGE			BIT(0)
#define BIT_DOWN				BIT(1)
#define BIT_MOVE				BIT(2)
#define BIT_UP					BIT(3)
#define BIT_PALM				BIT(4)
#define BIT_PALM_REJECT				BIT(5)
#define BIT_PT_EXIST				BIT(11)
#define BIT_MUST_ZERO				BIT(13)
#define BIT_ICON_EVENT				BIT(15)

/* Per-finger sub_status flags */
#define SUB_BIT_EXIST				BIT(0)
#define SUB_BIT_DOWN				BIT(1)
#define SUB_BIT_MOVE				BIT(2)
#define SUB_BIT_UP				BIT(3)

#define MAX_SUPPORTED_FINGER_NUM		5
#define MAX_SUPPORTED_BUTTON_NUM		8

#define DEFAULT_TOUCH_POINT_MODE		2
#define ZINITIX_DEFAULT_MAX_X			1200
#define ZINITIX_DEFAULT_MAX_Y			1920

#define CHIP_ON_DELAY				15 /* ms */
#define FIRMWARE_ON_DELAY			20 /* ms */

struct zinitix_chip_data {
	u8 i2c_retries;
	u16 reset_delay_ms;
	bool keep_powered_idle;
};

struct point_coord {
	__le16	x;
	__le16	y;
	u8	width;
	u8	sub_status;
	u8	minor_width;
	u8	angle;
} __packed;

struct touch_event {
	__le16	status;
	u8	finger_mask;
	u8	time_stamp;
	struct point_coord point_coord[MAX_SUPPORTED_FINGER_NUM];
} __packed;

enum {
	ZINITIX_SUPPLY_VDD,
	ZINITIX_SUPPLY_VCCA,
	ZINITIX_NUM_SUPPLIES
};

static const char * const zinitix_supply_names[] = {
	[ZINITIX_SUPPLY_VDD] = "vdd",
	[ZINITIX_SUPPLY_VCCA] = "vcca",
};

struct bt541_ts_data {
	struct i2c_client *client;
	struct input_dev *input_dev;
	struct touchscreen_properties prop;
	struct regulator_bulk_data supplies[ZINITIX_NUM_SUPPLIES];
	const struct zinitix_chip_data *chip_data;
	u32 keycodes[MAX_SUPPORTED_BUTTON_NUM];
	int num_keycodes;
	u32 zinitix_mode;
	bool have_versioninfo;
	bool keep_powered_initialized;
	bool sleeping;
	u16 chip_revision;
	u16 firmware_version;
	u16 regdata_version;
};

static int zinitix_i2c_send(struct bt541_ts_data *bt541, const u8 *data, int len)
{
	int tries = bt541->chip_data->i2c_retries;
	int attempt, ret;

	for (attempt = 0; attempt < tries; attempt++) {
		ret = i2c_master_send(bt541->client, data, len);
		if (ret != -EREMOTEIO && ret != -EAGAIN && ret != -ECONNREFUSED)
			return ret;
		if (attempt + 1 < tries)
			usleep_range(1000, 1500);
	}

	return ret;
}

static int zinitix_read_data(struct bt541_ts_data *bt541,
			     u16 reg, void *values, size_t length)
{
	__le16 reg_le = cpu_to_le16(reg);
	int ret;

	/* A single i2c_transfer() transaction does not work here. */
	ret = zinitix_i2c_send(bt541, (u8 *)&reg_le, sizeof(reg_le));
	if (ret != sizeof(reg_le))
		return ret < 0 ? ret : -EIO;

	usleep_range(50, 60);

	ret = i2c_master_recv(bt541->client, (u8 *)values, length);
	if (ret != length)
		return ret < 0 ? ret : -EIO;

	usleep_range(10, 20);
	return 0;
}

static int zinitix_write_u16(struct bt541_ts_data *bt541, u16 reg, u16 value)
{
	__le16 packet[2] = { cpu_to_le16(reg), cpu_to_le16(value) };
	int ret;

	ret = zinitix_i2c_send(bt541, (u8 *)packet, sizeof(packet));
	if (ret != sizeof(packet))
		return ret < 0 ? ret : -EIO;

	usleep_range(10, 20);
	return 0;
}

static int zinitix_write_cmd(struct bt541_ts_data *bt541, u16 reg)
{
	__le16 reg_le = cpu_to_le16(reg);
	int ret;

	ret = zinitix_i2c_send(bt541, (u8 *)&reg_le, sizeof(reg_le));
	if (ret != sizeof(reg_le))
		return ret < 0 ? ret : -EIO;

	usleep_range(10, 20);
	return 0;
}

static int zinitix_read_u16(struct bt541_ts_data *bt541, u16 reg, u16 *val)
{
	__le16 le_val;
	int ret;

	ret = zinitix_read_data(bt541, reg, &le_val, sizeof(le_val));
	if (ret)
		return ret;

	*val = le16_to_cpu(le_val);
	return 0;
}

static int zinitix_read_version_info(struct bt541_ts_data *bt541)
{
	struct i2c_client *client = bt541->client;
	int error;

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
	dev_info(&client->dev,
		 "chip revision %04x firmware version %04x regdata version %04x\n",
		 bt541->chip_revision, bt541->firmware_version,
		 bt541->regdata_version);

	return 0;
}

static int zinitix_init_touch(struct bt541_ts_data *bt541)
{
	struct i2c_client *client = bt541->client;
	u16 int_flags;
	int i, error;

	error = zinitix_write_cmd(bt541, ZINITIX_SWRESET_CMD);
	if (error) {
		dev_err(&client->dev, "Failed to write reset command\n");
		return error;
	}

	if (bt541->chip_data->reset_delay_ms)
		msleep(bt541->chip_data->reset_delay_ms);

	if (!bt541->have_versioninfo) {
		error = zinitix_read_version_info(bt541);
		if (error)
			return error;
	}

	error = zinitix_write_u16(bt541, ZINITIX_INT_ENABLE_FLAG, 0);
	if (error)
		return error;

	error = zinitix_write_cmd(bt541, ZINITIX_SWRESET_CMD);
	if (error)
		return error;

	if (bt541->chip_data->reset_delay_ms)
		msleep(bt541->chip_data->reset_delay_ms);

	error = zinitix_write_u16(bt541, ZINITIX_X_RESOLUTION,
				  bt541->prop.max_x);
	if (error)
		return error;

	error = zinitix_write_u16(bt541, ZINITIX_Y_RESOLUTION,
				  bt541->prop.max_y);
	if (error)
		return error;

	error = zinitix_write_u16(bt541, ZINITIX_BUTTON_SUPPORTED_NUM,
				  bt541->num_keycodes);
	if (error)
		return error;

	error = zinitix_write_u16(bt541, ZINITIX_SUPPORTED_FINGER_NUM,
				  MAX_SUPPORTED_FINGER_NUM);
	if (error)
		return error;

	error = zinitix_write_u16(bt541, ZINITIX_INITIAL_TOUCH_MODE,
				  bt541->zinitix_mode);
	if (error)
		return error;

	error = zinitix_write_u16(bt541, ZINITIX_TOUCH_MODE,
				  bt541->zinitix_mode);
	if (error)
		return error;

	if (bt541->zinitix_mode == 0) {
		error = zinitix_write_cmd(bt541, ZINITIX_CALIBRATE_CMD);
		if (error)
			return error;
	}

	int_flags = BIT_PT_CNT_CHANGE | BIT_DOWN | BIT_MOVE | BIT_UP;
	if (bt541->num_keycodes)
		int_flags |= BIT_ICON_EVENT;

	error = zinitix_write_u16(bt541, ZINITIX_INT_ENABLE_FLAG, int_flags);
	if (error)
		return error;

	for (i = 0; i < 10; i++) {
		zinitix_write_cmd(bt541, ZINITIX_CLEAR_INT_STATUS_CMD);
		usleep_range(10, 20);
	}

	return 0;
}

static int zinitix_send_power_on_sequence(struct bt541_ts_data *bt541)
{
	struct i2c_client *client = bt541->client;
	u16 chip_code = 0;
	int error;

	error = zinitix_write_u16(bt541, ZINITIX_VENDOR_CMD_ENABLE, 0x0001);
	if (error) {
		dev_err(&client->dev,
			"Failed to send power sequence (vendor cmd enable)\n");
		return error;
	}
	usleep_range(10, 20);

	error = zinitix_read_u16(bt541, ZINITIX_CHIP_CODE_REG, &chip_code);
	if (error) {
		dev_err(&client->dev, "Failed to read chip code\n");
		return error;
	}
	dev_dbg(&client->dev, "chip code = 0x%04x\n", chip_code);
	usleep_range(10, 20);

	error = zinitix_write_cmd(bt541, ZINITIX_INTN_CLEAR);
	if (error) {
		dev_err(&client->dev,
			"Failed to send power sequence (intn clear)\n");
		return error;
	}
	usleep_range(10, 20);

	error = zinitix_write_u16(bt541, ZINITIX_NVM_INIT, 0x0001);
	if (error) {
		dev_err(&client->dev,
			"Failed to send power sequence (nvm init)\n");
		return error;
	}
	mdelay(2);

	error = zinitix_write_u16(bt541, ZINITIX_PROGRAM_START, 0x0001);
	if (error) {
		dev_err(&client->dev,
			"Failed to send power sequence (program start)\n");
		return error;
	}

	msleep(FIRMWARE_ON_DELAY);
	return 0;
}

static void zinitix_report_keys(struct bt541_ts_data *bt541, u16 icon_events)
{
	int i;

	for (i = 0; i < bt541->num_keycodes; i++) {
		u32 code = bt541->keycodes[i];

		if (code == KEY_RESERVED)
			continue;

		if (icon_events & BIT(i))
			input_report_key(bt541->input_dev, code, 1);
		if (icon_events & BIT(i + 8))
			input_report_key(bt541->input_dev, code, 0);
	}
}

static void zinitix_report_finger(struct bt541_ts_data *bt541, int slot,
				  const struct point_coord *p)
{
	bool active;
	u16 x, y;

	if (bt541->zinitix_mode == 0) {
		active = !!(p->sub_status & SUB_BIT_EXIST);
	} else {
		if (unlikely(!(p->sub_status &
			       (SUB_BIT_UP | SUB_BIT_DOWN | SUB_BIT_MOVE)))) {
			dev_dbg(&bt541->client->dev,
				"unknown finger event %#02x\n", p->sub_status);
			return;
		}
		active = !(p->sub_status & SUB_BIT_UP);
	}

	x = le16_to_cpu(p->x);
	y = le16_to_cpu(p->y);

	if (active && (x > bt541->prop.max_x || y > bt541->prop.max_y))
		return;

	input_mt_slot(bt541->input_dev, slot);
	if (input_mt_report_slot_state(bt541->input_dev, MT_TOOL_FINGER,
				       active)) {
		touchscreen_report_pos(bt541->input_dev, &bt541->prop,
				       x, y, true);
		input_report_abs(bt541->input_dev, ABS_MT_TOUCH_MAJOR,
				 p->width);
		input_report_abs(bt541->input_dev, ABS_MT_WIDTH_MAJOR,
				 p->width);
	}
}

static irqreturn_t zinitix_ts_irq_handler(int irq, void *dev_id)
{
	struct bt541_ts_data *bt541 = dev_id;
	struct i2c_client *client = bt541->client;
	struct touch_event touch_event;
	unsigned long finger_mask;
	u16 status;
	int error;
	int i;

	memset(&touch_event, 0, sizeof(touch_event));

	if (bt541->zinitix_mode == 0) {
		u8 raw[4 + 6 * MAX_SUPPORTED_FINGER_NUM];

		error = zinitix_read_data(bt541, ZINITIX_POINT_STATUS_REG,
					  raw, sizeof(raw));
		if (!error) {
			memcpy(&touch_event, raw, 4);
			for (i = 0; i < MAX_SUPPORTED_FINGER_NUM; i++)
				memcpy(&touch_event.point_coord[i],
				       raw + 4 + i * 6, 6);
		}
	} else {
		error = zinitix_read_data(bt541, ZINITIX_POINT_STATUS_REG,
					  &touch_event, sizeof(touch_event));
	}

	if (error) {
		dev_err(&client->dev, "Failed to read touchpoint struct: %d\n",
			error);
		goto out;
	}

	status = le16_to_cpu(touch_event.status);
	if (!status || status == 0xffff || (status & BIT_MUST_ZERO))
		goto out;

	if ((status & BIT_ICON_EVENT) && bt541->num_keycodes) {
		u16 icon_events;

		error = zinitix_read_u16(bt541, ZINITIX_ICON_STATUS_REG,
					 &icon_events);
		if (error) {
			dev_err(&client->dev, "Failed to read icon status: %d\n",
				error);
			goto out;
		}
		zinitix_report_keys(bt541, icon_events);
	}

	if (bt541->zinitix_mode == 0) {
		if (status & BIT_PT_EXIST) {
			for (i = 0; i < MAX_SUPPORTED_FINGER_NUM; i++)
				zinitix_report_finger(bt541, i,
						      &touch_event.point_coord[i]);
		} else {
			for (i = 0; i < MAX_SUPPORTED_FINGER_NUM; i++) {
				input_mt_slot(bt541->input_dev, i);
				input_mt_report_slot_state(bt541->input_dev,
							   MT_TOOL_FINGER,
							   false);
			}
		}
	} else {
		finger_mask = touch_event.finger_mask;
		for_each_set_bit(i, &finger_mask, MAX_SUPPORTED_FINGER_NUM)
			zinitix_report_finger(bt541, i,
					      &touch_event.point_coord[i]);
	}

	input_mt_sync_frame(bt541->input_dev);
	input_sync(bt541->input_dev);

out:
	zinitix_write_cmd(bt541, ZINITIX_CLEAR_INT_STATUS_CMD);
	return IRQ_HANDLED;
}

static int zinitix_start(struct bt541_ts_data *bt541)
{
	int error;

	error = regulator_bulk_enable(ARRAY_SIZE(bt541->supplies),
				      bt541->supplies);
	if (error) {
		dev_err(&bt541->client->dev,
			"Failed to enable regulators: %d\n", error);
		return error;
	}

	msleep(CHIP_ON_DELAY);

	if (!bt541->keep_powered_initialized) {
		if (bt541->chip_data->keep_powered_idle) {
			zinitix_write_cmd(bt541, ZINITIX_WAKEUP_CMD);
			msleep(FIRMWARE_ON_DELAY);
		}
		error = zinitix_send_power_on_sequence(bt541);
		if (!error && bt541->chip_data->keep_powered_idle)
			bt541->keep_powered_initialized = true;
		if (error)
			goto disable_supplies;
	} else if (bt541->sleeping) {
		error = zinitix_write_cmd(bt541, ZINITIX_WAKEUP_CMD);
		if (error)
			goto disable_supplies;
		msleep(FIRMWARE_ON_DELAY);
	}

	error = zinitix_init_touch(bt541);
	if (error) {
		dev_err(&bt541->client->dev,
			"Error while configuring touch IC: %d\n", error);
		goto disable_supplies;
	}

	bt541->sleeping = false;
	enable_irq(bt541->client->irq);

	return 0;

disable_supplies:
	regulator_bulk_disable(ARRAY_SIZE(bt541->supplies), bt541->supplies);
	return error;
}

static int zinitix_stop(struct bt541_ts_data *bt541)
{
	int i, error;

	disable_irq(bt541->client->irq);

	if (bt541->chip_data->keep_powered_idle) {
		error = zinitix_write_u16(bt541, ZINITIX_INT_ENABLE_FLAG, 0);
		if (!error)
			error = zinitix_write_cmd(bt541, ZINITIX_IDLE_CMD);
		if (error) {
			enable_irq(bt541->client->irq);
			return error;
		}
		msleep(FIRMWARE_ON_DELAY);
		bt541->sleeping = true;

		for (i = 0; i < MAX_SUPPORTED_FINGER_NUM; i++) {
			input_mt_slot(bt541->input_dev, i);
			input_mt_report_slot_state(bt541->input_dev,
						   MT_TOOL_FINGER, false);
		}
		for (i = 0; i < bt541->num_keycodes; i++) {
			if (bt541->keycodes[i] != KEY_RESERVED)
				input_report_key(bt541->input_dev,
						 bt541->keycodes[i], 0);
		}
		input_mt_sync_frame(bt541->input_dev);
		input_sync(bt541->input_dev);
	}

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

static int zinitix_init_regulators(struct bt541_ts_data *bt541)
{
	struct device *dev = &bt541->client->dev;
	int i;

	for (i = 0; i < ZINITIX_NUM_SUPPLIES; i++)
		bt541->supplies[i].supply = zinitix_supply_names[i];

	return devm_regulator_bulk_get(dev, ZINITIX_NUM_SUPPLIES,
				       bt541->supplies);
}

static int zinitix_probe_chip(struct bt541_ts_data *bt541)
{
	int error;

	error = regulator_bulk_enable(ARRAY_SIZE(bt541->supplies),
				      bt541->supplies);
	if (error)
		return error;

	msleep(CHIP_ON_DELAY);

	if (bt541->chip_data->keep_powered_idle) {
		zinitix_write_cmd(bt541, ZINITIX_WAKEUP_CMD);
		msleep(FIRMWARE_ON_DELAY);
	}

	error = zinitix_send_power_on_sequence(bt541);
	if (error)
		goto err_disable;

	if (bt541->chip_data->keep_powered_idle)
		bt541->keep_powered_initialized = true;

	error = zinitix_write_cmd(bt541, ZINITIX_SWRESET_CMD);
	if (error)
		goto err_disable;

	if (bt541->chip_data->reset_delay_ms)
		msleep(bt541->chip_data->reset_delay_ms);

	error = zinitix_read_version_info(bt541);
	if (error)
		goto err_disable;

	zinitix_write_u16(bt541, ZINITIX_INT_ENABLE_FLAG, 0);
	if (bt541->chip_data->keep_powered_idle) {
		if (!zinitix_write_cmd(bt541, ZINITIX_IDLE_CMD)) {
			msleep(FIRMWARE_ON_DELAY);
			bt541->sleeping = true;
		}
	}

err_disable:
	regulator_bulk_disable(ARRAY_SIZE(bt541->supplies), bt541->supplies);
	return error;
}

static const struct zinitix_chip_data zinitix_bt400_data = {
	.i2c_retries = 1,
	.reset_delay_ms = 0,
	.keep_powered_idle = false,
};

static const struct zinitix_chip_data zinitix_bt532_data = {
	.i2c_retries = 8,
	.reset_delay_ms = CHIP_ON_DELAY,
	.keep_powered_idle = true,
};

static int zinitix_ts_probe(struct i2c_client *client,
			    const struct i2c_device_id *id)
{
	struct bt541_ts_data *bt541;
	struct input_dev *input_dev;
	int n_keycodes;
	int error;
	int i;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		dev_err(&client->dev,
			"Failed to assert adapter's support for plain I2C\n");
		return -ENXIO;
	}

	bt541 = devm_kzalloc(&client->dev, sizeof(*bt541), GFP_KERNEL);
	if (!bt541)
		return -ENOMEM;

	bt541->client = client;
	bt541->chip_data = of_device_get_match_data(&client->dev);
	if (!bt541->chip_data && id && id->driver_data)
		bt541->chip_data = (const struct zinitix_chip_data *)id->driver_data;
	if (!bt541->chip_data)
		bt541->chip_data = &zinitix_bt400_data;

	error = zinitix_init_regulators(bt541);
	if (error) {
		if (error != -EPROBE_DEFER)
			dev_err(&client->dev,
				"Failed to get regulators: %d\n", error);
		return error;
	}

	error = device_property_read_u32(&client->dev, "zinitix,mode",
					 &bt541->zinitix_mode);
	if (error < 0)
		bt541->zinitix_mode = DEFAULT_TOUCH_POINT_MODE;

	if (bt541->zinitix_mode != 0 && bt541->zinitix_mode != 2) {
		dev_err(&client->dev,
			"Malformed zinitix,mode property, must be 0 or 2 (supplied: %u)\n",
			bt541->zinitix_mode);
		return -EINVAL;
	}

	n_keycodes = device_property_read_u32_array(&client->dev,
						    "linux,keycodes", NULL, 0);
	if (n_keycodes > 0) {
		if (n_keycodes > MAX_SUPPORTED_BUTTON_NUM) {
			dev_err(&client->dev,
				"Too many keycodes (%d > %d)\n",
				n_keycodes, MAX_SUPPORTED_BUTTON_NUM);
			return -EINVAL;
		}
		error = device_property_read_u32_array(&client->dev,
						       "linux,keycodes",
						       bt541->keycodes,
						       n_keycodes);
		if (error)
			return error;
		bt541->num_keycodes = n_keycodes;
	}

	input_dev = devm_input_allocate_device(&client->dev);
	if (!input_dev)
		return -ENOMEM;

	bt541->input_dev = input_dev;
	input_dev->name = "Zinitix Capacitive TouchScreen";
	input_dev->phys = "input/ts";
	input_dev->id.bustype = BUS_I2C;
	input_dev->open = zinitix_input_open;
	input_dev->close = zinitix_input_close;

	__set_bit(INPUT_PROP_DIRECT, input_dev->propbit);

	input_set_abs_params(input_dev, ABS_MT_POSITION_X,
			     0, ZINITIX_DEFAULT_MAX_X, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_POSITION_Y,
			     0, ZINITIX_DEFAULT_MAX_Y, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_WIDTH_MAJOR, 0, 255, 0, 0);

	touchscreen_parse_properties(input_dev, true, &bt541->prop);

	error = input_mt_init_slots(input_dev, MAX_SUPPORTED_FINGER_NUM,
				    INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED);
	if (error) {
		dev_err(&client->dev, "Failed to initialize MT slots: %d\n",
			error);
		return error;
	}

	for (i = 0; i < bt541->num_keycodes; i++) {
		if (bt541->keycodes[i] != KEY_RESERVED)
			input_set_capability(input_dev, EV_KEY,
					     bt541->keycodes[i]);
	}

	input_set_drvdata(input_dev, bt541);
	i2c_set_clientdata(client, bt541);

	error = zinitix_probe_chip(bt541);
	if (error) {
		dev_err(&client->dev, "Failed to probe touchscreen IC: %d\n",
			error);
		return error;
	}

	error = devm_request_threaded_irq(&client->dev, client->irq,
					  NULL, zinitix_ts_irq_handler,
					  IRQF_ONESHOT, client->name, bt541);
	if (error) {
		dev_err(&client->dev, "Failed to request IRQ %d: %d\n",
			client->irq, error);
		return error;
	}

	disable_irq(client->irq);

	error = input_register_device(input_dev);
	if (error) {
		dev_err(&client->dev, "Failed to register input device: %d\n",
			error);
		return error;
	}

	return 0;
}

static int __maybe_unused zinitix_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct bt541_ts_data *bt541 = i2c_get_clientdata(client);

	mutex_lock(&bt541->input_dev->mutex);
	if (bt541->input_dev->users)
		zinitix_stop(bt541);
	mutex_unlock(&bt541->input_dev->mutex);

	return 0;
}

static int __maybe_unused zinitix_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct bt541_ts_data *bt541 = i2c_get_clientdata(client);
	int ret = 0;

	mutex_lock(&bt541->input_dev->mutex);
	if (bt541->input_dev->users)
		ret = zinitix_start(bt541);
	mutex_unlock(&bt541->input_dev->mutex);

	return ret;
}

static SIMPLE_DEV_PM_OPS(zinitix_pm_ops, zinitix_suspend, zinitix_resume);

#ifdef CONFIG_OF
static const struct of_device_id zinitix_of_match[] = {
	{ .compatible = "zinitix,bt400", .data = &zinitix_bt400_data },
	{ .compatible = "zinitix,bt532", .data = &zinitix_bt532_data },
	{ .compatible = "zinitix,bt541", .data = &zinitix_bt400_data },
	{ }
};
MODULE_DEVICE_TABLE(of, zinitix_of_match);
#endif

static const struct i2c_device_id zinitix_id[] = {
	{ "bt400", (kernel_ulong_t)&zinitix_bt400_data },
	{ "bt532", (kernel_ulong_t)&zinitix_bt532_data },
	{ "bt541", (kernel_ulong_t)&zinitix_bt400_data },
	{ }
};
MODULE_DEVICE_TABLE(i2c, zinitix_id);

static struct i2c_driver zinitix_ts_driver = {
	.driver = {
		.name = "Zinitix-TS",
		.of_match_table = of_match_ptr(zinitix_of_match),
		.pm = &zinitix_pm_ops,
	},
	.probe = zinitix_ts_probe,
	.id_table = zinitix_id,
};
module_i2c_driver(zinitix_ts_driver);

MODULE_AUTHOR("Michael Srba <Michael.Srba@seznam.cz>");
MODULE_DESCRIPTION("Zinitix touchscreen driver");
MODULE_LICENSE("GPL v2");
