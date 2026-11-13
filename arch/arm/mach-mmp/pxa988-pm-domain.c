// SPDX-License-Identifier: GPL-2.0
/*
 * Marvell PXA988/PXA986 3D GPU (GC) power domain driver.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/regulator/consumer.h>
#include <linux/spinlock.h>

#define GC_ACLK_RST		BIT(0)
#define GC_FCLK_RST		BIT(1)
#define GC_HCLK_RST		BIT(2)
#define GC_ALL_RST		(GC_ACLK_RST | GC_FCLK_RST | GC_HCLK_RST)
#define GC_HWMODE		BIT(11)

#define GC_AUTO_PWR_ON		BIT(0)
#define GC_PWR_STAT		BIT(0)
#define APMU_PWR_BLK_TMR_VAL	0x20001fff

static DEFINE_SPINLOCK(apmu_pwr_lock);

struct pxa988_gc_pd {
	struct generic_pm_domain genpd;
	struct device *dev;
	void __iomem *apmu_gc;
	void __iomem *pwr_ctrl;
	void __iomem *pwr_tmr;
	void __iomem *pwr_stat;
	struct clk_bulk_data clks[3];
	struct regulator *supply;
};

static inline struct pxa988_gc_pd *to_pxa988_gc_pd(struct generic_pm_domain *gpd)
{
	return container_of(gpd, struct pxa988_gc_pd, genpd);
}

static int pxa988_gc_power_on(struct generic_pm_domain *genpd)
{
	struct pxa988_gc_pd *pd = to_pxa988_gc_pd(genpd);
	unsigned long flags;
	u32 val;
	int ret;

	if (pd->supply) {
		ret = regulator_enable(pd->supply);
		if (ret) {
			dev_err(pd->dev, "failed to enable supply: %d\n", ret);
			return ret;
		}
	}

	ret = clk_bulk_prepare_enable(ARRAY_SIZE(pd->clks), pd->clks);
	if (ret) {
		dev_err(pd->dev, "failed to enable clocks: %d\n", ret);
		goto err_supply;
	}

	writel_relaxed(APMU_PWR_BLK_TMR_VAL, pd->pwr_tmr);

	val = readl_relaxed(pd->apmu_gc);
	val |= GC_HWMODE;
	writel_relaxed(val, pd->apmu_gc);

	spin_lock_irqsave(&apmu_pwr_lock, flags);
	val = readl_relaxed(pd->pwr_ctrl);
	val |= GC_AUTO_PWR_ON;
	writel_relaxed(val, pd->pwr_ctrl);
	spin_unlock_irqrestore(&apmu_pwr_lock, flags);

	ret = readl_poll_timeout_atomic(pd->pwr_stat, val, val & GC_PWR_STAT,
					10, 10000);
	if (ret) {
		dev_err(pd->dev, "timed out waiting for GC power-on\n");
		goto err_pwr_ctrl;
	}

	val = readl_relaxed(pd->apmu_gc);
	val |= GC_ALL_RST;
	writel_relaxed(val, pd->apmu_gc);
	usleep_range(10, 20);

	return 0;

err_pwr_ctrl:
	spin_lock_irqsave(&apmu_pwr_lock, flags);
	val = readl_relaxed(pd->pwr_ctrl);
	val &= ~GC_AUTO_PWR_ON;
	writel_relaxed(val, pd->pwr_ctrl);
	spin_unlock_irqrestore(&apmu_pwr_lock, flags);

	clk_bulk_disable_unprepare(ARRAY_SIZE(pd->clks), pd->clks);
err_supply:
	if (pd->supply)
		regulator_disable(pd->supply);
	return ret;
}

static int pxa988_gc_power_off(struct generic_pm_domain *genpd)
{
	struct pxa988_gc_pd *pd = to_pxa988_gc_pd(genpd);
	unsigned long flags;
	u32 val;
	int ret;

	val = readl_relaxed(pd->apmu_gc);
	val &= ~GC_ALL_RST;
	writel_relaxed(val, pd->apmu_gc);

	clk_bulk_disable_unprepare(ARRAY_SIZE(pd->clks), pd->clks);

	spin_lock_irqsave(&apmu_pwr_lock, flags);
	val = readl_relaxed(pd->pwr_ctrl);
	val &= ~GC_AUTO_PWR_ON;
	writel_relaxed(val, pd->pwr_ctrl);
	spin_unlock_irqrestore(&apmu_pwr_lock, flags);

	ret = readl_poll_timeout_atomic(pd->pwr_stat, val,
					!(val & GC_PWR_STAT), 10, 10000);
	if (ret)
		dev_warn(pd->dev, "timed out waiting for GC power-off\n");

	if (pd->supply)
		regulator_disable(pd->supply);

	return 0;
}

static void __iomem *pxa988_ioremap_res(struct platform_device *pdev,
					const char *name)
{
	struct resource *res;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);
	if (!res)
		return IOMEM_ERR_PTR(-EINVAL);

	return devm_ioremap(&pdev->dev, res->start, resource_size(res));
}

static int pxa988_gc_pd_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pxa988_gc_pd *pd;
	int ret;

	pd = devm_kzalloc(dev, sizeof(*pd), GFP_KERNEL);
	if (!pd)
		return -ENOMEM;

	pd->dev = dev;
	pd->apmu_gc = pxa988_ioremap_res(pdev, "apmu-gc");
	if (IS_ERR(pd->apmu_gc))
		return PTR_ERR(pd->apmu_gc);

	pd->pwr_ctrl = pxa988_ioremap_res(pdev, "pwr-ctrl");
	if (IS_ERR(pd->pwr_ctrl))
		return PTR_ERR(pd->pwr_ctrl);

	pd->pwr_tmr = pxa988_ioremap_res(pdev, "pwr-tmr");
	if (IS_ERR(pd->pwr_tmr))
		return PTR_ERR(pd->pwr_tmr);

	pd->pwr_stat = pxa988_ioremap_res(pdev, "pwr-stat");
	if (IS_ERR(pd->pwr_stat))
		return PTR_ERR(pd->pwr_stat);

	pd->clks[0].id = "reg";
	pd->clks[1].id = "bus";
	pd->clks[2].id = "core";
	ret = devm_clk_bulk_get(dev, ARRAY_SIZE(pd->clks), pd->clks);
	if (ret)
		return ret;

	pd->supply = devm_regulator_get_optional(dev, "power");
	if (IS_ERR(pd->supply)) {
		ret = PTR_ERR(pd->supply);
		if (ret != -ENODEV)
			return ret;
		pd->supply = NULL;
	}

	pd->genpd.name = "pxa988-gc";
	pd->genpd.power_on = pxa988_gc_power_on;
	pd->genpd.power_off = pxa988_gc_power_off;

	pm_genpd_init(&pd->genpd, NULL, true);
	ret = of_genpd_add_provider_simple(dev->of_node, &pd->genpd);
	if (ret) {
		pm_genpd_remove(&pd->genpd);
		return ret;
	}

	return 0;
}

static const struct of_device_id pxa988_gc_pd_of_match[] = {
	{ .compatible = "marvell,pxa988-gc-power" },
	{ /* sentinel */ }
};

static struct platform_driver pxa988_gc_pd_driver = {
	.driver = {
		.name = "pxa988-gc-power",
		.of_match_table = pxa988_gc_pd_of_match,
		.suppress_bind_attrs = true,
	},
	.probe = pxa988_gc_pd_probe,
};
builtin_platform_driver(pxa988_gc_pd_driver);
