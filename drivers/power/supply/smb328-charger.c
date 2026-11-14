// SPDX-License-Identifier: GPL-2.0-only
/*
 * Summit Microelectronics SMB328 Battery Charger driver
 *
 * Derived from Samsung goyawifi K34 smb328_charger.c and KML t110-smb328.c,
 * using standard power_supply and extcon interfaces.
 */

#include <linux/extcon.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/power_supply.h>
#include <linux/workqueue.h>

#define SMB328_CHARGE_CURRENT		0x00
#define SMB328_INPUT_CURRENT_LIMIT	0x01
#define SMB328_FLOAT_VOLTAGE		0x02
#define SMB328_FUNC_CTRL_A		0x03
#define SMB328_FUNC_CTRL_B		0x04
#define SMB328_FUNC_CTRL_C		0x05
#define SMB328_OTG_LDO_CTRL		0x06
#define SMB328_VARIOUS_FUNCS		0x07
#define SMB328_IRQ_CLEAR		0x30
#define SMB328_COMMAND			0x31
#define SMB328_STATUS_C			0x36
#define SMB328_IRQ_STATUS_C		0x37

#define SMB328_CMD_ALLOW_WRITE		BIT(7)
#define SMB328_CMD_CHARGE_DISABLE	BIT(4)
#define SMB328_CMD_AC_MODE		BIT(3)
#define SMB328_CMD_USB_MODE		BIT(2)

#define SMB328_STAT_C_ENABLED		BIT(0)
#define SMB328_STAT_C_CHARGING_MASK	(BIT(1) | BIT(2))
#define SMB328_STAT_C_ERROR_MASK	(BIT(4) | BIT(5))
#define SMB328_STAT_C_TERM_MASK		(BIT(6) | BIT(7))

#define SMB328_IRQ_C_WATCHDOG		BIT(7)
#define SMB328_IRQ_C_FAULT_MASK		(BIT(1) | BIT(2))

#define SMB328_POLL_INTERVAL		(2 * HZ)
#define SMB328_SAFETY_TIMEOUT		(6 * 60 * 60 * HZ)

#define SMB328_DEFAULT_FLOAT_UV		4320000
#define SMB328_OVP_LIMIT_UV		4400000
#define SMB328_TEMP_HIGH_DECI		450
#define SMB328_TEMP_HIGH_RECOV_DECI	420
#define SMB328_TEMP_LOW_DECI		0
#define SMB328_TEMP_LOW_RECOV_DECI	30

struct smb328_charger {
	struct i2c_client *client;
	struct device *dev;
	struct extcon_dev *edev;
	struct notifier_block extcon_nb;
	struct gpio_desc *en_gpio;
	struct power_supply *psy;
	struct power_supply *battery;
	struct mutex lock;
	struct delayed_work poll_work;

	u32 dcp_input_ua;
	u32 dcp_fast_ua;
	u32 float_uv;

	bool online;
	bool hot;
	bool cold;
	bool expired;
	bool charge_on;
	bool configured;

	int status;
	int health;
	int charge_type;
	int usb_type;
	int input_ua;
	int fast_ua;
	int last_error;
	unsigned long session_start;
};

static int smb328_read(struct smb328_charger *chg, u8 reg)
{
	return i2c_smbus_read_byte_data(chg->client, reg);
}

static int smb328_write(struct smb328_charger *chg, u8 reg, u8 val)
{
	int ret, rb;

	ret = i2c_smbus_write_byte_data(chg->client, reg, val);
	if (ret < 0)
		return ret;

	rb = smb328_read(chg, reg);
	if (rb < 0)
		return rb;

	return rb == val ? 0 : -EIO;
}

static u8 smb328_encode_current(u32 ua)
{
	u32 ma = ua / 1000;

	if (ma <= 500)
		return 0;
	if (ma >= 1200)
		return 7;
	return (ma - 500) / 100;
}

static u8 smb328_encode_float_uv(u32 uv)
{
	u32 mv = clamp_val(uv / 1000, 3460U, 4360U);

	return (mv - 3460) / 20;
}

static int smb328_configure(struct smb328_charger *chg, bool dedicated)
{
	u32 target_in_ua = dedicated ? chg->dcp_input_ua : 500000;
	u32 target_fast_ua = dedicated ? chg->dcp_fast_ua : 500000;
	u8 in_code = smb328_encode_current(target_in_ua);
	u8 fast_code = smb328_encode_current(target_fast_ua);
	u8 vfloat_code = smb328_encode_float_uv(chg->float_uv);
	int cmd, v, ret, close_ret;

	cmd = smb328_read(chg, SMB328_COMMAND);
	if (cmd < 0)
		return cmd;

	/* Open configuration write gate, disable charging & boost */
	cmd = (cmd | SMB328_CMD_ALLOW_WRITE | SMB328_CMD_CHARGE_DISABLE) &
	      ~(SMB328_CMD_AC_MODE | SMB328_CMD_USB_MODE);
	ret = smb328_write(chg, SMB328_COMMAND, cmd);
	if (ret)
		return ret;

	ret = smb328_write(chg, SMB328_FUNC_CTRL_C, 0x00);
	if (ret)
		goto close;

	ret = smb328_write(chg, SMB328_FUNC_CTRL_B, 0x6e);
	if (ret)
		goto close;

	ret = smb328_write(chg, SMB328_FUNC_CTRL_A, 0xda);
	if (ret)
		goto close;

	v = smb328_read(chg, SMB328_FLOAT_VOLTAGE);
	if (v < 0) {
		ret = v;
		goto close;
	}
	ret = smb328_write(chg, SMB328_FLOAT_VOLTAGE,
			   (v & ~0x7e) | (vfloat_code << 1));
	if (ret)
		goto close;

	v = smb328_read(chg, SMB328_CHARGE_CURRENT);
	if (v < 0) {
		ret = v;
		goto close;
	}
	/* Set fast charge current and 200 mA termination current */
	ret = smb328_write(chg, SMB328_CHARGE_CURRENT,
			   (v & 0x18) | (fast_code << 5) | 0x07);
	if (ret)
		goto close;

	v = smb328_read(chg, SMB328_INPUT_CURRENT_LIMIT);
	if (v < 0) {
		ret = v;
		goto close;
	}
	/* Set input current limit, AICL enabled at 4.25 V */
	ret = smb328_write(chg, SMB328_INPUT_CURRENT_LIMIT,
			   (v & 0x18) | (in_code << 5));
	if (ret)
		goto close;

	v = smb328_read(chg, SMB328_VARIOUS_FUNCS);
	if (v < 0) {
		ret = v;
		goto close;
	}
	ret = smb328_write(chg, SMB328_VARIOUS_FUNCS, (v & 0x1f) | 0xe0);
	if (ret)
		goto close;

	v = smb328_read(chg, SMB328_OTG_LDO_CTRL);
	if (v < 0) {
		ret = v;
		goto close;
	}
	/* Never source VBUS */
	ret = smb328_write(chg, SMB328_OTG_LDO_CTRL, v & ~BIT(5));
	if (ret)
		goto close;

	cmd |= dedicated ? (SMB328_CMD_AC_MODE | SMB328_CMD_USB_MODE) :
			   SMB328_CMD_AC_MODE;

close:
	close_ret = smb328_write(chg, SMB328_COMMAND,
				 cmd & ~SMB328_CMD_ALLOW_WRITE);
	if (!ret)
		ret = close_ret;

	chg->charge_on = false;
	if (!ret) {
		chg->configured = true;
		chg->input_ua = target_in_ua;
		chg->fast_ua = target_fast_ua;
	}
	return ret;
}

static int smb328_set_enable(struct smb328_charger *chg, bool on)
{
	int cmd, ret;

	cmd = smb328_read(chg, SMB328_COMMAND);
	if (cmd < 0)
		return cmd;

	cmd &= ~SMB328_CMD_ALLOW_WRITE;
	if (on)
		cmd &= ~SMB328_CMD_CHARGE_DISABLE;
	else
		cmd |= SMB328_CMD_CHARGE_DISABLE;

	ret = smb328_write(chg, SMB328_COMMAND, cmd);
	if (!ret)
		chg->charge_on = on;

	return ret;
}

static void smb328_detect_cable(struct smb328_charger *chg, bool *online,
				int *usb_type)
{
	if (!chg->edev) {
		*online = false;
		*usb_type = POWER_SUPPLY_USB_TYPE_UNKNOWN;
		return;
	}

	if (extcon_get_state(chg->edev, EXTCON_CHG_USB_DCP) > 0) {
		*online = true;
		*usb_type = POWER_SUPPLY_USB_TYPE_DCP;
	} else if (extcon_get_state(chg->edev, EXTCON_CHG_USB_CDP) > 0) {
		*online = true;
		*usb_type = POWER_SUPPLY_USB_TYPE_CDP;
	} else if (extcon_get_state(chg->edev, EXTCON_CHG_USB_SDP) > 0 ||
		   extcon_get_state(chg->edev, EXTCON_USB) > 0) {
		*online = true;
		*usb_type = POWER_SUPPLY_USB_TYPE_SDP;
	} else {
		*online = false;
		*usb_type = POWER_SUPPLY_USB_TYPE_UNKNOWN;
	}
}

static int smb328_get_battery_info(struct smb328_charger *chg,
				   int *temp_deci, int *voltage_uv)
{
	union power_supply_propval val;
	int ret;

	if (!chg->battery) {
		chg->battery = power_supply_get_by_phandle(chg->dev->of_node,
							   "monitored-battery");
		if (!chg->battery)
			chg->battery = power_supply_get_by_name("battery");
		if (!chg->battery)
			return -EPROBE_DEFER;
	}

	ret = power_supply_get_property(chg->battery, POWER_SUPPLY_PROP_TEMP,
					&val);
	if (ret)
		return ret;
	*temp_deci = val.intval;

	ret = power_supply_get_property(chg->battery,
					POWER_SUPPLY_PROP_VOLTAGE_AVG, &val);
	if (ret)
		return ret;
	*voltage_uv = val.intval;

	return 0;
}

static void smb328_update(struct smb328_charger *chg)
{
	bool online, dedicated, on;
	int usb_type, temp_deci, voltage_uv;
	int stat, fault, ret;
	u32 target_in_ua;

	smb328_detect_cable(chg, &online, &usb_type);
	dedicated = online && (usb_type == POWER_SUPPLY_USB_TYPE_DCP);

	ret = smb328_get_battery_info(chg, &temp_deci, &voltage_uv);
	if (ret) {
		/* Defer safely with charging disabled until battery is ready */
		smb328_set_enable(chg, false);
		chg->online = online;
		chg->usb_type = usb_type;
		chg->status = online ? POWER_SUPPLY_STATUS_NOT_CHARGING :
				       POWER_SUPPLY_STATUS_DISCHARGING;
		chg->health = POWER_SUPPLY_HEALTH_UNKNOWN;
		chg->charge_type = POWER_SUPPLY_CHARGE_TYPE_NONE;
		chg->last_error = ret;
		return;
	}

	if (online && !chg->online) {
		chg->session_start = jiffies;
		chg->expired = false;
		ret = smb328_set_enable(chg, false);
		if (ret)
			goto fail;
		ret = i2c_smbus_write_byte_data(chg->client, SMB328_IRQ_CLEAR,
						0xff);
		if (ret < 0)
			goto fail;
		chg->configured = false;
	}
	if (!online) {
		chg->expired = false;
		chg->session_start = 0;
	}

	chg->online = online;
	chg->usb_type = usb_type;
	target_in_ua = dedicated ? chg->dcp_input_ua : 500000;

	if (!chg->configured || chg->input_ua != target_in_ua) {
		ret = smb328_configure(chg, dedicated);
		if (ret)
			goto fail;
	}

	if (temp_deci >= SMB328_TEMP_HIGH_DECI)
		chg->hot = true;
	else if (temp_deci <= SMB328_TEMP_HIGH_RECOV_DECI)
		chg->hot = false;

	if (temp_deci <= SMB328_TEMP_LOW_DECI)
		chg->cold = true;
	else if (temp_deci >= SMB328_TEMP_LOW_RECOV_DECI)
		chg->cold = false;

	if (online &&
	    time_after(jiffies, chg->session_start + SMB328_SAFETY_TIMEOUT))
		chg->expired = true;

	fault = smb328_read(chg, SMB328_IRQ_STATUS_C);
	if (fault < 0) {
		ret = fault;
		goto fail;
	}

	if (online && (fault & SMB328_IRQ_C_WATCHDOG)) {
		ret = smb328_set_enable(chg, false);
		if (ret)
			goto fail;
		ret = i2c_smbus_write_byte_data(chg->client, SMB328_IRQ_CLEAR,
						0xff);
		if (ret < 0)
			goto fail;
		ret = smb328_configure(chg, dedicated);
		if (ret)
			goto fail;
		fault = smb328_read(chg, SMB328_IRQ_STATUS_C);
		if (fault < 0) {
			ret = fault;
			goto fail;
		}
	}

	if (chg->hot)
		chg->health = POWER_SUPPLY_HEALTH_OVERHEAT;
	else if (chg->cold)
		chg->health = POWER_SUPPLY_HEALTH_COLD;
	else if (online && (fault & SMB328_IRQ_C_FAULT_MASK))
		chg->health = POWER_SUPPLY_HEALTH_UNSPEC_FAILURE;
	else if (chg->expired)
		chg->health = POWER_SUPPLY_HEALTH_SAFETY_TIMER_EXPIRE;
	else if (online && (fault & SMB328_IRQ_C_WATCHDOG))
		chg->health = POWER_SUPPLY_HEALTH_WATCHDOG_TIMER_EXPIRE;
	else
		chg->health = POWER_SUPPLY_HEALTH_GOOD;

	on = online && chg->health == POWER_SUPPLY_HEALTH_GOOD &&
	     voltage_uv < SMB328_OVP_LIMIT_UV;

	ret = smb328_set_enable(chg, on);
	if (ret)
		goto fail;

	stat = smb328_read(chg, SMB328_STATUS_C);
	if (stat < 0) {
		ret = stat;
		goto fail;
	}

	if (!online) {
		chg->status = POWER_SUPPLY_STATUS_DISCHARGING;
		chg->charge_type = POWER_SUPPLY_CHARGE_TYPE_NONE;
	} else if (!on) {
		chg->status = POWER_SUPPLY_STATUS_NOT_CHARGING;
		chg->charge_type = POWER_SUPPLY_CHARGE_TYPE_NONE;
	} else if (stat & SMB328_STAT_C_TERM_MASK) {
		chg->status = POWER_SUPPLY_STATUS_FULL;
		chg->charge_type = POWER_SUPPLY_CHARGE_TYPE_NONE;
	} else if ((stat & SMB328_STAT_C_ENABLED) &&
		   !(stat & SMB328_STAT_C_ERROR_MASK) &&
		   (stat & SMB328_STAT_C_CHARGING_MASK)) {
		chg->status = POWER_SUPPLY_STATUS_CHARGING;
		chg->charge_type = ((stat & SMB328_STAT_C_CHARGING_MASK) == BIT(1)) ?
				   POWER_SUPPLY_CHARGE_TYPE_TRICKLE :
				   POWER_SUPPLY_CHARGE_TYPE_FAST;
	} else {
		chg->status = POWER_SUPPLY_STATUS_NOT_CHARGING;
		chg->charge_type = POWER_SUPPLY_CHARGE_TYPE_NONE;
	}

	if (voltage_uv >= SMB328_OVP_LIMIT_UV)
		chg->health = POWER_SUPPLY_HEALTH_OVERVOLTAGE;

	chg->last_error = 0;
	return;

fail:
	smb328_set_enable(chg, false);
	chg->configured = false;
	chg->last_error = ret;
	chg->status = POWER_SUPPLY_STATUS_UNKNOWN;
	chg->health = POWER_SUPPLY_HEALTH_UNSPEC_FAILURE;
	chg->charge_type = POWER_SUPPLY_CHARGE_TYPE_NONE;
}

static void smb328_poll_work(struct work_struct *work)
{
	struct smb328_charger *chg = container_of(to_delayed_work(work),
						  struct smb328_charger,
						  poll_work);
	int old_status, old_health, old_usb_type;

	mutex_lock(&chg->lock);
	old_status = chg->status;
	old_health = chg->health;
	old_usb_type = chg->usb_type;

	smb328_update(chg);

	if (old_status != chg->status || old_health != chg->health ||
	    old_usb_type != chg->usb_type) {
		power_supply_changed(chg->psy);
		if (chg->battery)
			power_supply_changed(chg->battery);
	}
	mutex_unlock(&chg->lock);

	schedule_delayed_work(&chg->poll_work, SMB328_POLL_INTERVAL);
}

static int smb328_extcon_notifier(struct notifier_block *nb,
				  unsigned long event, void *ptr)
{
	struct smb328_charger *chg = container_of(nb, struct smb328_charger,
						  extcon_nb);

	mod_delayed_work(system_wq, &chg->poll_work, 0);
	return NOTIFY_OK;
}

static enum power_supply_property smb328_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_USB_TYPE,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_CHARGE_TYPE,
	POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE,
};

static enum power_supply_usb_type smb328_usb_types[] = {
	POWER_SUPPLY_USB_TYPE_UNKNOWN,
	POWER_SUPPLY_USB_TYPE_SDP,
	POWER_SUPPLY_USB_TYPE_CDP,
	POWER_SUPPLY_USB_TYPE_DCP,
};

static int smb328_get_property(struct power_supply *psy,
			       enum power_supply_property psp,
			       union power_supply_propval *val)
{
	struct smb328_charger *chg = power_supply_get_drvdata(psy);
	int ret = 0;

	mutex_lock(&chg->lock);
	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = chg->online;
		break;
	case POWER_SUPPLY_PROP_USB_TYPE:
		val->intval = chg->usb_type;
		break;
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = chg->status;
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		val->intval = chg->health;
		break;
	case POWER_SUPPLY_PROP_CHARGE_TYPE:
		val->intval = chg->charge_type;
		break;
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		val->intval = chg->input_ua;
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
		val->intval = chg->fast_ua;
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE:
		val->intval = chg->float_uv;
		break;
	default:
		ret = -EINVAL;
		break;
	}
	mutex_unlock(&chg->lock);

	return ret;
}

static const struct power_supply_desc smb328_psy_desc = {
	.name		= "smb328-charger",
	.type		= POWER_SUPPLY_TYPE_USB,
	.properties	= smb328_props,
	.num_properties	= ARRAY_SIZE(smb328_props),
	.get_property	= smb328_get_property,
	.usb_types	= smb328_usb_types,
	.num_usb_types	= ARRAY_SIZE(smb328_usb_types),
};

static void smb328_stop(void *data)
{
	struct smb328_charger *chg = data;

	cancel_delayed_work_sync(&chg->poll_work);
	mutex_lock(&chg->lock);
	smb328_set_enable(chg, false);
	mutex_unlock(&chg->lock);
	if (chg->battery)
		power_supply_put(chg->battery);
}

static int smb328_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct power_supply_config psy_cfg = {};
	struct smb328_charger *chg;
	int ret;

	chg = devm_kzalloc(dev, sizeof(*chg), GFP_KERNEL);
	if (!chg)
		return -ENOMEM;

	chg->client = client;
	chg->dev = dev;
	mutex_init(&chg->lock);
	INIT_DELAYED_WORK(&chg->poll_work, smb328_poll_work);

	/* Conservative <= 500 mA defaults unless overridden in DT */
	chg->dcp_input_ua = 500000;
	chg->dcp_fast_ua = 500000;
	chg->float_uv = SMB328_DEFAULT_FLOAT_UV;

	device_property_read_u32(dev, "summit,dcp-input-current-microamp",
				 &chg->dcp_input_ua);
	device_property_read_u32(dev, "summit,fast-charge-current-microamp",
				 &chg->dcp_fast_ua);
	device_property_read_u32(dev, "summit,float-voltage-microvolt",
				 &chg->float_uv);

	chg->en_gpio = devm_gpiod_get_optional(dev, "enable", GPIOD_OUT_LOW);
	if (IS_ERR(chg->en_gpio))
		return PTR_ERR(chg->en_gpio);

	if (of_property_read_bool(dev->of_node, "extcon")) {
		chg->edev = extcon_get_edev_by_phandle(dev, 0);
		if (IS_ERR(chg->edev))
			return PTR_ERR(chg->edev);
	}

	/* Verify chip presence on I2C bus */
	ret = smb328_read(chg, SMB328_COMMAND);
	if (ret < 0)
		return ret;

	i2c_set_clientdata(client, chg);
	psy_cfg.drv_data = chg;
	psy_cfg.of_node = dev->of_node;

	chg->psy = devm_power_supply_register(dev, &smb328_psy_desc, &psy_cfg);
	if (IS_ERR(chg->psy))
		return PTR_ERR(chg->psy);

	ret = devm_add_action_or_reset(dev, smb328_stop, chg);
	if (ret)
		return ret;

	if (chg->edev) {
		chg->extcon_nb.notifier_call = smb328_extcon_notifier;
		ret = devm_extcon_register_notifier_all(dev, chg->edev,
							&chg->extcon_nb);
		if (ret)
			return ret;
	}

	schedule_delayed_work(&chg->poll_work, 0);
	dev_info(dev, "SMB328 charger registered (DCP %u uA, float %u uV)\n",
		 chg->dcp_input_ua, chg->float_uv);

	return 0;
}

static void smb328_shutdown(struct i2c_client *client)
{
	smb328_stop(i2c_get_clientdata(client));
}

static const struct i2c_device_id smb328_id[] = {
	{ "smb328", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, smb328_id);

static const struct of_device_id smb328_of_match[] = {
	{ .compatible = "summit,smb328" },
	{ }
};
MODULE_DEVICE_TABLE(of, smb328_of_match);

static struct i2c_driver smb328_driver = {
	.driver = {
		.name = "smb328-charger",
		.of_match_table = smb328_of_match,
	},
	.probe = smb328_probe,
	.shutdown = smb328_shutdown,
	.id_table = smb328_id,
};
module_i2c_driver(smb328_driver);

MODULE_DESCRIPTION("Summit Microelectronics SMB328 charger driver");
MODULE_LICENSE("GPL");
