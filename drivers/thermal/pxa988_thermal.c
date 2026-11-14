// SPDX-License-Identifier: GPL-2.0-only
/*
 * Marvell PXA988/PXA986 DROTS thermal sensor driver
 *
 * Polling-based high-range SoC temperature sensor (80000 mC floor, 2500 mC step).
 * Derived from K34 drivers/thermal/pxa988_thermal.c and KML pxa98x-thermal.c.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/thermal.h>

#define TS_CTRL			0x20
#define TS_READ			0x24

#define TS_CTRL_HW_AUTO_EN	BIT(29)
#define TS_CTRL_WARN_MASK	BIT(25)
#define TS_CTRL_ON_INT_MASK	BIT(18)
#define TS_CTRL_TEMP_ON		BIT(3)
#define TS_CTRL_RST_N		BIT(2)
#define TS_CTRL_LOW_RANGE	BIT(1)
#define TS_CTRL_CHOP_EN		BIT(0)

#define TS_READ_TEMP_ON		BIT(4)
#define TS_READ_OUT_MASK	0x0f

#define PXA988_THERMAL_FLOOR_MC	80000
#define PXA988_THERMAL_STEP_MC	2500
#define PXA988_THERMAL_RETRIES	10

struct pxa988_thermal {
	void __iomem *regs;
	struct clk *clk;
	struct mutex lock;
};

static int pxa988_thermal_get_temp(void *data, int *temp)
{
	struct pxa988_thermal *t = data;
	u32 raw, ctrl, gray, binary;
	int i, ret = 0;

	mutex_lock(&t->lock);
	for (i = 0; i < PXA988_THERMAL_RETRIES; i++) {
		raw = readl(t->regs + TS_READ);
		if (raw & TS_READ_TEMP_ON)
			break;
		msleep(20);
	}

	if (i == PXA988_THERMAL_RETRIES) {
		ret = -EAGAIN;
		goto out;
	}

	gray = raw & TS_READ_OUT_MASK;
	binary = gray ^ (gray >> 1) ^ (gray >> 2) ^ (gray >> 3);
	*temp = PXA988_THERMAL_FLOOR_MC + binary * PXA988_THERMAL_STEP_MC;

	ctrl = readl(t->regs + TS_CTRL);
	writel(ctrl & ~TS_CTRL_TEMP_ON, t->regs + TS_CTRL);
	writel(ctrl | TS_CTRL_TEMP_ON, t->regs + TS_CTRL);

out:
	mutex_unlock(&t->lock);
	return ret;
}

static const struct thermal_zone_of_device_ops pxa988_thermal_ops = {
	.get_temp = pxa988_thermal_get_temp,
};

static void pxa988_thermal_clk_disable(void *data)
{
	clk_disable_unprepare(data);
}

static int pxa988_thermal_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pxa988_thermal *t;
	struct thermal_zone_device *tz;
	struct resource *res;
	u32 ctrl;
	int ret;

	t = devm_kzalloc(dev, sizeof(*t), GFP_KERNEL);
	if (!t)
		return -ENOMEM;

	mutex_init(&t->lock);
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	t->regs = devm_ioremap_resource(dev, res);
	if (IS_ERR(t->regs))
		return PTR_ERR(t->regs);

	t->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(t->clk))
		return PTR_ERR(t->clk);

	ret = clk_prepare_enable(t->clk);
	if (ret)
		return ret;

	ret = devm_add_action_or_reset(dev, pxa988_thermal_clk_disable, t->clk);
	if (ret)
		return ret;

	/* Reset and start DROTS high-range polling, preserving over-range reset */
	ctrl = readl(t->regs + TS_CTRL);
	ctrl &= ~(TS_CTRL_HW_AUTO_EN | TS_CTRL_WARN_MASK | TS_CTRL_ON_INT_MASK |
		  TS_CTRL_TEMP_ON | TS_CTRL_RST_N | TS_CTRL_LOW_RANGE);
	ctrl |= TS_CTRL_CHOP_EN;
	writel(ctrl, t->regs + TS_CTRL);

	msleep(2000);
	writel(ctrl | TS_CTRL_RST_N | TS_CTRL_TEMP_ON, t->regs + TS_CTRL);

	tz = devm_thermal_zone_of_sensor_register(dev, 0, t,
						  &pxa988_thermal_ops);
	if (IS_ERR(tz))
		return PTR_ERR(tz);

	dev_info(dev, "PXA988 DROTS thermal sensor registered (80C floor, 2.5C step)\n");
	return 0;
}

static const struct of_device_id pxa988_thermal_of_match[] = {
	{ .compatible = "marvell,pxa988-thermal" },
	{ }
};
MODULE_DEVICE_TABLE(of, pxa988_thermal_of_match);

static struct platform_driver pxa988_thermal_driver = {
	.driver = {
		.name = "pxa988-thermal",
		.of_match_table = pxa988_thermal_of_match,
	},
	.probe = pxa988_thermal_probe,
};
module_platform_driver(pxa988_thermal_driver);

MODULE_DESCRIPTION("Marvell PXA988/PXA986 DROTS thermal sensor driver");
MODULE_LICENSE("GPL");
