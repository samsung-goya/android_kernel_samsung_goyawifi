// SPDX-License-Identifier: GPL-2.0-only
/*
 * Marvell 88PM822 software fuel-gauge battery driver
 *
 * Uses 88PM822 GPADC (VBAT, GPADC2 NTC) and base-page scratch registers
 * (0xdd, 0xde, 0xe0, 0xee) to maintain a two-RC OCV fuel model across reboots.
 * Current is inferred from the electrical model, not directly measured.
 */

#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/mfd/88pm80x.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>

#include "pm822-battery-tables.h"

#define PM822_GPADC_BIAS_REG	0x0d
#define PM822_VBAT_MEAS1	0x42
#define PM822_GPADC2_MEAS1	0x58
#define PM822_VBAT_AVG1		0xa0

#define PM822_FG_V1_REG		0xdd
#define PM822_FG_V2_REG		0xde
#define PM822_FG_FRAC_REG	0xe0
#define PM822_FG_SOC_REG	0xee

#define PM822_FUEL_POLL_INTERVAL	(5 * HZ)
#define PM822_FUEL_STORE_INTERVAL	(60 * HZ)

struct pm822_fuel_model {
	s64 soc;	/* micro-percent, 0..100000000 */
	int v1_uv;
	int v2_uv;
	int rs_mohm;
	int rp_mohm;
	int current_ua;
};

struct pm822_battery {
	struct device *dev;
	struct regmap *base;
	struct regmap *gpadc;
	struct mutex lock;
	struct power_supply *psy;
	struct power_supply *charger;
	struct delayed_work fuel_work;
	struct pm822_fuel_model fuel;
	unsigned long last_store;
	u64 last_fuel_ms;
};

static int fg_clamp(int val, int low, int high)
{
	return clamp(val, low, high);
}

static int fg_lerp(int a, int b, int x, int x0, int x1)
{
	return a + (b - a) * (x - x0) / (x1 - x0);
}

static int fg_rtot(bool charging, int current_ma, int temp_deci, int soc)
{
	int n = charging ? 4 : 6;
	int nt = charging ? 4 : 5;
	int t0 = charging ? -50 : -200;
	const int *bins = charging ? chg_ib : dischg_ib;
	int idx = fg_clamp(soc, 0, 99);
	int i, j, a, b;

	current_ma = fg_clamp(current_ma, bins[0], bins[n - 1]);
	temp_deci = fg_clamp(temp_deci, t0, t0 + (nt - 1) * 150);

	for (i = 1; i < n - 1 && current_ma > bins[i]; i++)
		;
	for (j = 1; j < nt - 1 && temp_deci > t0 + j * 150; j++)
		;

	if (charging) {
		a = fg_lerp(chg_rtot[i - 1][j - 1][idx],
			    chg_rtot[i][j - 1][idx],
			    current_ma, bins[i - 1], bins[i]);
		b = fg_lerp(chg_rtot[i - 1][j][idx],
			    chg_rtot[i][j][idx],
			    current_ma, bins[i - 1], bins[i]);
	} else {
		a = fg_lerp(dis_chg_rtot[i - 1][j - 1][idx],
			    dis_chg_rtot[i][j - 1][idx],
			    current_ma, bins[i - 1], bins[i]);
		b = fg_lerp(dis_chg_rtot[i - 1][j][idx],
			    dis_chg_rtot[i][j][idx],
			    current_ma, bins[i - 1], bins[i]);
	}

	return fg_clamp(fg_lerp(a, b, temp_deci,
				t0 + (j - 1) * 150, t0 + j * 150),
			30, 6000);
}

static int fg_percent(const struct pm822_fuel_model *m)
{
	return fg_clamp(div_s64(m->soc + 500000, 1000000), 0, 100);
}

static void fg_seed(struct pm822_fuel_model *m, int percent, int v1_uv,
		    int v2_uv)
{
	m->soc = (s64)fg_clamp(percent, 0, 100) * 1000000;
	m->v1_uv = v1_uv;
	m->v2_uv = v2_uv;
	m->rs_mohm = 120;
	m->rp_mohm = 40;
	m->current_ua = 0;
}

static void fg_step(struct pm822_fuel_model *m, int voltage_uv, int temp_deci,
		    int elapsed_ms, bool full)
{
	int percent = div_s64(m->soc, 1000000);
	int ocv_uv, rtot, ib_ua, dt;

	if (percent >= 100) {
		ocv_uv = pm822_ocv_table[100] * 1000;
	} else {
		int delta = pm822_ocv_table[percent + 1] -
			    pm822_ocv_table[percent];

		ocv_uv = pm822_ocv_table[percent] * 1000 +
			 div_s64((s64)delta * (m->soc - (s64)percent * 1000000),
				 1000);
	}

	ib_ua = div_s64((s64)(ocv_uv - voltage_uv - m->v1_uv - m->v2_uv) * 1000,
			m->rs_mohm);
	ib_ua = fg_clamp(ib_ua, -3000000, 3000000);

	rtot = fg_rtot(ib_ua < 0, ib_ua < 0 ? -ib_ua / 1000 : ib_ua / 1000,
		       temp_deci, percent);
	m->rp_mohm = rtot / 3;
	m->rs_mohm = rtot / 3 + 32;

	dt = fg_clamp(elapsed_ms, 0, 5000);
	m->v1_uv += div_s64((div_s64((s64)ib_ua * m->rp_mohm, 1000) -
			     m->v1_uv) * dt, 1000 * m->rp_mohm);
	m->v2_uv += div_s64((div_s64((s64)ib_ua * m->rp_mohm, 1000) -
			     m->v2_uv) * dt, 10000 * m->rp_mohm);
	m->soc -= div_s64((s64)ib_ua * dt * 100, 12960000);

	if (m->soc < 0)
		m->soc = 0;
	if (m->soc > 100000000)
		m->soc = 100000000;
	if (full && voltage_uv >= 4250000)
		m->soc = 100000000;

	m->current_ua = ib_ua;
}

static int pm822_adc_read_raw(struct pm822_battery *bat, unsigned int reg)
{
	u8 buf[2];
	int ret;

	ret = regmap_bulk_read(bat->gpadc, reg, buf, 2);
	if (ret)
		return ret;

	return (buf[0] << 4) | (buf[1] & 0x0f);
}

static int pm822_read_vbat_uv(struct pm822_battery *bat, unsigned int reg,
			      int *uv)
{
	int raw = pm822_adc_read_raw(bat, reg);

	if (raw < 0)
		return raw;
	if (!raw || raw == 4095)
		return -ENODATA;

	*uv = DIV_ROUND_CLOSEST(raw * 5600000LL, 4096);
	return 0;
}

static int pm822_read_temp_deci(struct pm822_battery *bat, int *temp_deci)
{
	static const u8 biases[] = { 6, 12, 3, 2, 1 };
	unsigned int old;
	int i, j, raw, mv, resistance, ret = -ENODATA;

	mutex_lock(&bat->lock);
	for (i = 0; i < ARRAY_SIZE(biases); i++) {
		ret = regmap_read(bat->gpadc, PM822_GPADC_BIAS_REG, &old);
		if (ret)
			goto out;
		if ((old & 0x0f) != biases[i]) {
			ret = regmap_update_bits(bat->gpadc,
						 PM822_GPADC_BIAS_REG,
						 0x0f, biases[i]);
			if (ret)
				goto out;
			msleep(50);
		}

		raw = pm822_adc_read_raw(bat, PM822_GPADC2_MEAS1);
		if (raw < 0) {
			ret = raw;
			goto out;
		}

		mv = (raw * 700) >> 11;
		if (mv <= 300 || mv >= 1250)
			continue;

		resistance = mv * 10000 / (biases[i] * 5 + 1);
		if (resistance > pm822_ntc_table[0] ||
		    resistance < pm822_ntc_table[ARRAY_SIZE(pm822_ntc_table) - 1])
			continue;

		for (j = 0; j < ARRAY_SIZE(pm822_ntc_table); j++) {
			if (resistance >= pm822_ntc_table[j])
				break;
		}

		ret = j - 25;
		if (ret < 50)
			ret -= 3;
		*temp_deci = ret * 10;
		ret = 0;
		goto out;
	}
	ret = -ENODATA;

out:
	mutex_unlock(&bat->lock);
	return ret;
}

static int pm822_fuel_store(struct pm822_battery *bat)
{
	unsigned int old;
	int p = div_s64(bat->fuel.soc, 1000000);
	int frac = div_s64(bat->fuel.soc - (s64)p * 1000000, 100000);
	int ret;

	ret = regmap_read(bat->base, PM822_FG_SOC_REG, &old);
	if (ret)
		return ret;

	ret = regmap_write(bat->base, PM822_FG_V1_REG,
			   fg_clamp(bat->fuel.v1_uv / 3000, -32, 31) & 0x3f);
	if (ret)
		return ret;

	ret = regmap_write(bat->base, PM822_FG_V2_REG,
			   fg_clamp(bat->fuel.v2_uv / 3000, -32, 31) & 0x3f);
	if (ret)
		return ret;

	ret = regmap_write(bat->base, PM822_FG_FRAC_REG, frac);
	if (ret)
		return ret;

	return regmap_write(bat->base, PM822_FG_SOC_REG, (old & 0x80) | p);
}

static struct power_supply *pm822_get_charger(struct pm822_battery *bat)
{
	if (!bat->charger) {
		bat->charger = power_supply_get_by_phandle(bat->dev->of_node,
							   "power-supplies");
		if (!bat->charger)
			bat->charger = power_supply_get_by_name("smb328-charger");
	}
	return bat->charger;
}

static void pm822_fuel_work(struct work_struct *work)
{
	struct pm822_battery *bat = container_of(to_delayed_work(work),
						 struct pm822_battery,
						 fuel_work);
	struct power_supply *charger = pm822_get_charger(bat);
	union power_supply_propval status = {
		.intval = POWER_SUPPLY_STATUS_UNKNOWN,
	};
	int voltage_uv, temp_deci, old_pct, new_pct, ret;
	bool changed = false;
	u64 now;

	if (charger)
		power_supply_get_property(charger, POWER_SUPPLY_PROP_STATUS,
					  &status);

	ret = pm822_read_vbat_uv(bat, PM822_VBAT_AVG1, &voltage_uv);
	if (!ret)
		ret = pm822_read_temp_deci(bat, &temp_deci);

	if (!ret) {
		mutex_lock(&bat->lock);
		now = ktime_to_ms(ktime_get_boottime());
		old_pct = fg_percent(&bat->fuel);

		fg_step(&bat->fuel, voltage_uv, temp_deci,
			min_t(u64, 5000, now - bat->last_fuel_ms),
			status.intval == POWER_SUPPLY_STATUS_FULL);
		bat->last_fuel_ms = now;

		if (time_after(jiffies,
			       bat->last_store + PM822_FUEL_STORE_INTERVAL)) {
			if (!pm822_fuel_store(bat))
				bat->last_store = jiffies;
		}

		new_pct = fg_percent(&bat->fuel);
		changed = (old_pct != new_pct);
		mutex_unlock(&bat->lock);

		if (changed)
			power_supply_changed(bat->psy);
	}

	schedule_delayed_work(&bat->fuel_work, PM822_FUEL_POLL_INTERVAL);
}

static enum power_supply_property pm822_battery_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_AVG,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_MODEL_NAME,
};

static int pm822_battery_get_property(struct power_supply *psy,
				      enum power_supply_property psp,
				      union power_supply_propval *val)
{
	struct pm822_battery *bat = power_supply_get_drvdata(psy);
	struct power_supply *charger;

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
	case POWER_SUPPLY_PROP_HEALTH:
		charger = pm822_get_charger(bat);
		if (!charger) {
			val->intval = (psp == POWER_SUPPLY_PROP_STATUS) ?
				      POWER_SUPPLY_STATUS_UNKNOWN :
				      POWER_SUPPLY_HEALTH_UNKNOWN;
			return 0;
		}
		return power_supply_get_property(charger, psp, val);

	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		return 0;

	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		return 0;

	case POWER_SUPPLY_PROP_CAPACITY:
		mutex_lock(&bat->lock);
		val->intval = fg_percent(&bat->fuel);
		mutex_unlock(&bat->lock);
		return 0;

	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		return pm822_read_vbat_uv(bat, PM822_VBAT_MEAS1, &val->intval);

	case POWER_SUPPLY_PROP_VOLTAGE_AVG:
		return pm822_read_vbat_uv(bat, PM822_VBAT_AVG1, &val->intval);

	case POWER_SUPPLY_PROP_CURRENT_NOW:
		mutex_lock(&bat->lock);
		/* Linux power_supply: positive = charging, negative = discharging */
		val->intval = -bat->fuel.current_ua;
		mutex_unlock(&bat->lock);
		return 0;

	case POWER_SUPPLY_PROP_TEMP:
		return pm822_read_temp_deci(bat, &val->intval);

	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = "88PM822-SW-FG";
		return 0;

	default:
		return -EINVAL;
	}
}

static const struct power_supply_desc pm822_battery_desc = {
	.name		= "battery",
	.type		= POWER_SUPPLY_TYPE_BATTERY,
	.properties	= pm822_battery_props,
	.num_properties	= ARRAY_SIZE(pm822_battery_props),
	.get_property	= pm822_battery_get_property,
};

static void pm822_battery_stop(void *data)
{
	struct pm822_battery *bat = data;

	cancel_delayed_work_sync(&bat->fuel_work);
	mutex_lock(&bat->lock);
	pm822_fuel_store(bat);
	mutex_unlock(&bat->lock);
	if (bat->charger)
		power_supply_put(bat->charger);
}

static int pm822_battery_probe(struct platform_device *pdev)
{
	struct pm80x_chip *chip = dev_get_drvdata(pdev->dev.parent);
	struct power_supply_config psy_cfg = {};
	struct pm822_battery *bat;
	unsigned int saved, v1, v2, frac;
	int initial, ret;

	if (!chip || !chip->regmap || !chip->subchip ||
	    !chip->subchip->regmap_gpadc)
		return -ENODEV;

	bat = devm_kzalloc(&pdev->dev, sizeof(*bat), GFP_KERNEL);
	if (!bat)
		return -ENOMEM;

	bat->dev = &pdev->dev;
	bat->base = chip->regmap;
	bat->gpadc = chip->subchip->regmap_gpadc;
	mutex_init(&bat->lock);
	INIT_DELAYED_WORK(&bat->fuel_work, pm822_fuel_work);

	ret = regmap_read(bat->base, PM822_FG_SOC_REG, &saved);
	if (ret)
		return ret;
	ret = regmap_read(bat->base, PM822_FG_V1_REG, &v1);
	if (ret)
		return ret;
	ret = regmap_read(bat->base, PM822_FG_V2_REG, &v2);
	if (ret)
		return ret;
	ret = regmap_read(bat->base, PM822_FG_FRAC_REG, &frac);
	if (ret)
		return ret;

	initial = saved & 0x7f;
	if ((saved & 0x80) || initial > 100) {
		int uv, mv;

		ret = pm822_read_vbat_uv(bat, PM822_VBAT_AVG1, &uv);
		if (ret)
			return ret;
		mv = uv / 1000;
		for (initial = 0;
		     initial < 100 && mv >= pm822_ocv_table[initial + 1];
		     initial++)
			;
		v1 = 0;
		v2 = 0;
		frac = 0;
	}

	fg_seed(&bat->fuel, initial,
		sign_extend32(v1 & 0x3f, 5) * 3000,
		sign_extend32(v2 & 0x3f, 5) * 3000);
	if (initial < 100 && frac < 10)
		bat->fuel.soc += frac * 100000;

	bat->last_fuel_ms = ktime_to_ms(ktime_get_boottime());
	bat->last_store = jiffies;

	platform_set_drvdata(pdev, bat);
	psy_cfg.drv_data = bat;
	psy_cfg.of_node = pdev->dev.of_node;

	bat->psy = devm_power_supply_register(&pdev->dev, &pm822_battery_desc,
					      &psy_cfg);
	if (IS_ERR(bat->psy))
		return PTR_ERR(bat->psy);

	ret = devm_add_action_or_reset(&pdev->dev, pm822_battery_stop, bat);
	if (ret)
		return ret;

	schedule_delayed_work(&bat->fuel_work, PM822_FUEL_POLL_INTERVAL);
	dev_info(&pdev->dev, "88PM822 software fuel gauge seeded at %d%%\n",
		 initial);

	return 0;
}

static void pm822_battery_shutdown(struct platform_device *pdev)
{
	pm822_battery_stop(platform_get_drvdata(pdev));
}

static const struct of_device_id pm822_battery_of_match[] = {
	{ .compatible = "marvell,88pm822-battery" },
	{ }
};
MODULE_DEVICE_TABLE(of, pm822_battery_of_match);

static struct platform_driver pm822_battery_driver = {
	.driver = {
		.name = "88pm822-battery",
		.of_match_table = pm822_battery_of_match,
	},
	.probe = pm822_battery_probe,
	.shutdown = pm822_battery_shutdown,
};
module_platform_driver(pm822_battery_driver);

MODULE_DESCRIPTION("Marvell 88PM822 software fuel-gauge battery driver");
MODULE_LICENSE("GPL");
