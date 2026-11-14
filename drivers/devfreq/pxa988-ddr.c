// SPDX-License-Identifier: GPL-2.0
/*
 * Marvell PXA988 / PXA986 DDR devfreq driver
 *
 * Scales active LPDDR2 frequency (312 / 533 MHz) using bootloader-initialized
 * MCK4 timing tables, MCK4 performance counters, and standard OPP/regulator
 * frameworks.
 */

#include <linux/clk.h>
#include <linux/devfreq.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_opp.h>

#define MCK4_PERF_CTRL		0x0440
#define MCK4_PERF_CFG		0x0444
#define MCK4_PERF_EVT		0x0448
#define MCK4_PERF_CNT0		0x0450
#define MCK4_PERF_CNT1		0x0454
#define MCK4_PERF_CNT2		0x0458
#define MCK4_PERF_CNT3		0x045c

#define MCK4_PERF_START_VAL	0x84948180

struct pxa988_ddr_devfreq {
	struct device *dev;
	void __iomem *mc_base;
	struct clk *clk;
	struct opp_table *opp_table;
	struct devfreq *devfreq;
	struct devfreq_dev_profile profile;
	struct devfreq_simple_ondemand_data ondemand_data;
	struct mutex lock;
	u64 sample_start_ns;
};

static void pxa988_ddr_counter_reset(struct pxa988_ddr_devfreq *priv)
{
	writel_relaxed(0, priv->mc_base + MCK4_PERF_CTRL);
	writel_relaxed(0x10, priv->mc_base + MCK4_PERF_EVT);
	writel_relaxed(0, priv->mc_base + MCK4_PERF_CNT0);
	writel_relaxed(0, priv->mc_base + MCK4_PERF_CNT1);
	writel_relaxed(0, priv->mc_base + MCK4_PERF_CNT2);
	writel_relaxed(0, priv->mc_base + MCK4_PERF_CNT3);
	writel_relaxed(15, priv->mc_base + MCK4_PERF_CFG);
	writel_relaxed(MCK4_PERF_START_VAL, priv->mc_base + MCK4_PERF_CTRL);
	priv->sample_start_ns = ktime_get_ns();
}

static int pxa988_ddr_target(struct device *dev, unsigned long *freq, u32 flags)
{
	struct pxa988_ddr_devfreq *priv = dev_get_drvdata(dev);
	struct dev_pm_opp *opp;
	unsigned long target_freq;
	int ret;

	opp = devfreq_recommended_opp(dev, freq, flags);
	if (IS_ERR(opp))
		return PTR_ERR(opp);

	target_freq = dev_pm_opp_get_freq(opp);
	dev_pm_opp_put(opp);

	mutex_lock(&priv->lock);
	ret = dev_pm_opp_set_rate(dev, target_freq);
	if (!ret)
		pxa988_ddr_counter_reset(priv);
	*freq = clk_get_rate(priv->clk);
	mutex_unlock(&priv->lock);

	return ret;
}

static int pxa988_ddr_get_dev_status(struct device *dev,
				     struct devfreq_dev_status *stat)
{
	struct pxa988_ddr_devfreq *priv = dev_get_drvdata(dev);
	u32 total;
	u64 busy;
	bool valid;

	mutex_lock(&priv->lock);
	writel_relaxed(0, priv->mc_base + MCK4_PERF_CTRL);
	total = readl_relaxed(priv->mc_base + MCK4_PERF_CNT0);
	busy = (u64)readl_relaxed(priv->mc_base + MCK4_PERF_CNT2) * 4;
	if (busy > total)
		busy = total;

	/* Reject >2s intervals so 32-bit MCK4 counters cannot wrap */
	valid = (ktime_get_ns() - priv->sample_start_ns) < 2ULL * NSEC_PER_SEC;
	stat->current_frequency = clk_get_rate(priv->clk);
	if (valid) {
		stat->total_time = total >> 8;
		stat->busy_time = (unsigned long)(busy >> 8);
	} else {
		stat->total_time = 0;
		stat->busy_time = 0;
	}

	pxa988_ddr_counter_reset(priv);
	mutex_unlock(&priv->lock);

	return 0;
}

static int pxa988_ddr_get_cur_freq(struct device *dev, unsigned long *freq)
{
	struct pxa988_ddr_devfreq *priv = dev_get_drvdata(dev);
	unsigned long rate = clk_get_rate(priv->clk);

	if (!rate)
		return -EIO;

	*freq = rate;
	return 0;
}

static int pxa988_ddr_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	static const char * const reg_names[] = { "ddr" };
	struct pxa988_ddr_devfreq *priv;
	struct resource *res;
	unsigned long init_freq;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;
	mutex_init(&priv->lock);
	platform_set_drvdata(pdev, priv);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	priv->mc_base = devm_ioremap_resource(dev, res);
	if (IS_ERR(priv->mc_base))
		return PTR_ERR(priv->mc_base);

	priv->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(priv->clk))
		return PTR_ERR(priv->clk);

	priv->opp_table = dev_pm_opp_set_regulators(dev, reg_names,
						    ARRAY_SIZE(reg_names));
	if (IS_ERR(priv->opp_table))
		return PTR_ERR(priv->opp_table);

	ret = dev_pm_opp_of_add_table(dev);
	if (ret) {
		dev_err(dev, "failed to add OPP table: %d\n", ret);
		goto err_put_reg;
	}

	init_freq = clk_get_rate(priv->clk);
	ret = dev_pm_opp_set_rate(dev, init_freq);
	if (ret) {
		dev_err(dev, "failed to vote initial OPP (%lu Hz): %d\n",
			init_freq, ret);
		goto err_rm_opp;
	}

	priv->profile.initial_freq = init_freq;
	priv->profile.polling_ms = 100;
	priv->profile.target = pxa988_ddr_target;
	priv->profile.get_dev_status = pxa988_ddr_get_dev_status;
	priv->profile.get_cur_freq = pxa988_ddr_get_cur_freq;

	priv->ondemand_data.upthreshold = 40;
	priv->ondemand_data.downdifferential = 10;

	pxa988_ddr_counter_reset(priv);

	priv->devfreq = devm_devfreq_add_device(dev, &priv->profile,
						DEVFREQ_GOV_SIMPLE_ONDEMAND,
						&priv->ondemand_data);
	if (IS_ERR(priv->devfreq)) {
		ret = PTR_ERR(priv->devfreq);
		goto err_rm_opp;
	}

	dev_info(dev, "PXA988 DDR devfreq registered (init %lu MHz)\n",
		 init_freq / 1000000);
	return 0;

err_rm_opp:
	dev_pm_opp_of_remove_table(dev);
err_put_reg:
	dev_pm_opp_put_regulators(priv->opp_table);
	return ret;
}

static int pxa988_ddr_remove(struct platform_device *pdev)
{
	struct pxa988_ddr_devfreq *priv = platform_get_drvdata(pdev);

	dev_pm_opp_of_remove_table(&pdev->dev);
	dev_pm_opp_put_regulators(priv->opp_table);
	return 0;
}

static const struct of_device_id pxa988_ddr_of_match[] = {
	{ .compatible = "marvell,pxa988-ddr" },
	{ }
};
MODULE_DEVICE_TABLE(of, pxa988_ddr_of_match);

static struct platform_driver pxa988_ddr_driver = {
	.probe = pxa988_ddr_probe,
	.remove = pxa988_ddr_remove,
	.driver = {
		.name = "pxa988-ddr-devfreq",
		.of_match_table = pxa988_ddr_of_match,
	},
};
module_platform_driver(pxa988_ddr_driver);

MODULE_DESCRIPTION("Marvell PXA988 DDR devfreq driver");
MODULE_LICENSE("GPL v2");
