// SPDX-License-Identifier: GPL-2.0
/*
 * Marvell PXA988/PXA986 3D GPU (GC) and VPU power domain driver.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/regulator/consumer.h>
#include <linux/spinlock.h>

#define APMU_PWR_BLK_TMR_VAL	0x20001fff

static DEFINE_SPINLOCK(apmu_pwr_lock);

struct pxa988_pd_desc {
	const char *name;
	const char *apmu_res_name;
	u32 hwmode_bit;
	u32 rst_bits;
	u32 pwr_bit;
	const char *clk_names[3];
	int num_clks;
};

struct pxa988_pd {
	struct generic_pm_domain genpd;
	struct device *dev;
	const struct pxa988_pd_desc *desc;
	void __iomem *apmu_reg;
	void __iomem *pwr_ctrl;
	void __iomem *pwr_tmr;
	void __iomem *pwr_stat;
	struct clk_bulk_data clks[3];
	struct regulator *supply;
};

static inline struct pxa988_pd *to_pxa988_pd(struct generic_pm_domain *gpd)
{
	return container_of(gpd, struct pxa988_pd, genpd);
}

static int pxa988_pd_power_on(struct generic_pm_domain *genpd)
{
	struct pxa988_pd *pd = to_pxa988_pd(genpd);
	const struct pxa988_pd_desc *desc = pd->desc;
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

	ret = clk_bulk_prepare_enable(desc->num_clks, pd->clks);
	if (ret) {
		dev_err(pd->dev, "failed to enable clocks: %d\n", ret);
		goto err_supply;
	}

	writel_relaxed(APMU_PWR_BLK_TMR_VAL, pd->pwr_tmr);

	val = readl_relaxed(pd->apmu_reg);
	val |= desc->hwmode_bit;
	writel_relaxed(val, pd->apmu_reg);

	spin_lock_irqsave(&apmu_pwr_lock, flags);
	val = readl_relaxed(pd->pwr_ctrl);
	val |= desc->pwr_bit;
	writel_relaxed(val, pd->pwr_ctrl);
	spin_unlock_irqrestore(&apmu_pwr_lock, flags);

	ret = readl_poll_timeout_atomic(pd->pwr_stat, val, val & desc->pwr_bit,
					10, 10000);
	if (ret) {
		dev_err(pd->dev, "timed out waiting for %s power-on\n",
			desc->name);
		goto err_pwr_ctrl;
	}

	val = readl_relaxed(pd->apmu_reg);
	val |= desc->rst_bits;
	writel_relaxed(val, pd->apmu_reg);
	usleep_range(10, 20);

	return 0;

err_pwr_ctrl:
	spin_lock_irqsave(&apmu_pwr_lock, flags);
	val = readl_relaxed(pd->pwr_ctrl);
	val &= ~desc->pwr_bit;
	writel_relaxed(val, pd->pwr_ctrl);
	spin_unlock_irqrestore(&apmu_pwr_lock, flags);

	clk_bulk_disable_unprepare(desc->num_clks, pd->clks);
err_supply:
	if (pd->supply)
		regulator_disable(pd->supply);
	return ret;
}

static int pxa988_pd_power_off(struct generic_pm_domain *genpd)
{
	struct pxa988_pd *pd = to_pxa988_pd(genpd);
	const struct pxa988_pd_desc *desc = pd->desc;
	unsigned long flags;
	u32 val;
	int ret;

	val = readl_relaxed(pd->apmu_reg);
	val &= ~desc->rst_bits;
	writel_relaxed(val, pd->apmu_reg);

	clk_bulk_disable_unprepare(desc->num_clks, pd->clks);

	spin_lock_irqsave(&apmu_pwr_lock, flags);
	val = readl_relaxed(pd->pwr_ctrl);
	val &= ~desc->pwr_bit;
	writel_relaxed(val, pd->pwr_ctrl);
	spin_unlock_irqrestore(&apmu_pwr_lock, flags);

	ret = readl_poll_timeout_atomic(pd->pwr_stat, val,
					!(val & desc->pwr_bit), 10, 10000);
	if (ret)
		dev_warn(pd->dev, "timed out waiting for %s power-off\n",
			 desc->name);

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

static int pxa988_pd_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct pxa988_pd_desc *desc;
	struct pxa988_pd *pd;
	int i, ret;

	desc = of_device_get_match_data(dev);
	if (!desc)
		return -EINVAL;

	pd = devm_kzalloc(dev, sizeof(*pd), GFP_KERNEL);
	if (!pd)
		return -ENOMEM;

	pd->dev = dev;
	pd->desc = desc;
	pd->apmu_reg = pxa988_ioremap_res(pdev, desc->apmu_res_name);
	if (IS_ERR(pd->apmu_reg))
		return PTR_ERR(pd->apmu_reg);

	pd->pwr_ctrl = pxa988_ioremap_res(pdev, "pwr-ctrl");
	if (IS_ERR(pd->pwr_ctrl))
		return PTR_ERR(pd->pwr_ctrl);

	pd->pwr_tmr = pxa988_ioremap_res(pdev, "pwr-tmr");
	if (IS_ERR(pd->pwr_tmr))
		return PTR_ERR(pd->pwr_tmr);

	pd->pwr_stat = pxa988_ioremap_res(pdev, "pwr-stat");
	if (IS_ERR(pd->pwr_stat))
		return PTR_ERR(pd->pwr_stat);

	for (i = 0; i < desc->num_clks; i++)
		pd->clks[i].id = desc->clk_names[i];
	ret = devm_clk_bulk_get(dev, desc->num_clks, pd->clks);
	if (ret)
		return ret;

	pd->supply = devm_regulator_get_optional(dev, "power");
	if (IS_ERR(pd->supply)) {
		ret = PTR_ERR(pd->supply);
		if (ret != -ENODEV)
			return ret;
		pd->supply = NULL;
	}

	pd->genpd.name = desc->name;
	pd->genpd.power_on = pxa988_pd_power_on;
	pd->genpd.power_off = pxa988_pd_power_off;

	pm_genpd_init(&pd->genpd, NULL, true);
	ret = of_genpd_add_provider_simple(dev->of_node, &pd->genpd);
	if (ret) {
		pm_genpd_remove(&pd->genpd);
		return ret;
	}

	return 0;
}

static const struct pxa988_pd_desc pxa988_gc_pd_desc = {
	.name = "pxa988-gc",
	.apmu_res_name = "apmu-gc",
	.hwmode_bit = BIT(11),
	.rst_bits = BIT(0) | BIT(1) | BIT(2),
	.pwr_bit = BIT(0),
	.clk_names = { "reg", "bus", "core" },
	.num_clks = 3,
};

static const struct pxa988_pd_desc pxa988_vpu_pd_desc = {
	.name = "pxa988-vpu",
	.apmu_res_name = "apmu-vpu",
	.hwmode_bit = BIT(19),
	.rst_bits = BIT(0) | BIT(1) | BIT(2),
	.pwr_bit = BIT(2),
	.clk_names = { "bus", "core" },
	.num_clks = 2,
};

static const struct of_device_id pxa988_pd_of_match[] = {
	{ .compatible = "marvell,pxa988-gc-power", .data = &pxa988_gc_pd_desc },
	{ .compatible = "marvell,pxa988-vpu-power", .data = &pxa988_vpu_pd_desc },
	{ /* sentinel */ }
};

static struct platform_driver pxa988_pd_driver = {
	.driver = {
		.name = "pxa988-pm-domain",
		.of_match_table = pxa988_pd_of_match,
		.suppress_bind_attrs = true,
	},
	.probe = pxa988_pd_probe,
};
builtin_platform_driver(pxa988_pd_driver);
