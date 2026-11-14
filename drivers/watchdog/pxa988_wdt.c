// SPDX-License-Identifier: GPL-2.0
/*
 * Marvell PXA988/PXA986 CP Timer Hardware Watchdog Driver
 *
 * Uses the 256 Hz CP watchdog timer (0xd4080000) to trigger a warm SoC
 * reset on hard lockup so DRAM contents (ramoops / last_kmsg) survive.
 */

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <linux/watchdog.h>

#define TMR_WMER		0x64
#define TMR_WMR			0x68
#define TMR_WVR			0x6c
#define TMR_WSR			0x70
#define TMR_WFAR		0x9c
#define TMR_WSAR		0xa0

#define TMR_WMER_WE		BIT(0)
#define TMR_WMER_WRIE		BIT(1)

#define MPMU_CPRR		0x0020
#define MPMU_CPRR_DSPR		BIT(2)
#define MPMU_CPRR_BBR		BIT(3)
#define MPMU_WDTPCR		0x0200
#define MPMU_APRR		0x1020
#define MPMU_APRR_CPR		BIT(0)
#define MPMU_APRR_WDTR		BIT(4)

#define PXA988_WDT_RATE		256
#define PXA988_WDT_DEFAULT_TO	20
#define PXA988_WDT_MAX_TO	120

static unsigned int timeout = PXA988_WDT_DEFAULT_TO;
module_param(timeout, uint, 0);
MODULE_PARM_DESC(timeout, "Watchdog timeout in seconds (default=20)");

static bool nowayout = WATCHDOG_NOWAYOUT;
module_param(nowayout, bool, 0);
MODULE_PARM_DESC(nowayout, "Watchdog cannot be stopped once started");

struct pxa988_wdt {
	void __iomem		*wdt;
	void __iomem		*mpmu;
	spinlock_t		lock;
	struct watchdog_device	wdd;
};

static inline void pxa988_wdt_unlock(void __iomem *wdt)
{
	writel_relaxed(0xbaba, wdt + TMR_WFAR);
	writel_relaxed(0xeb10, wdt + TMR_WSAR);
}

static void pxa988_wdt_hw_init(struct pxa988_wdt *drv)
{
	u32 reg;

	/* Hold CP and reset MSA so CP cannot cancel a watchdog reset */
	reg = readl_relaxed(drv->mpmu + MPMU_APRR) | MPMU_APRR_CPR | MPMU_APRR_WDTR;
	writel_relaxed(reg, drv->mpmu + MPMU_APRR);
	udelay(10);

	reg = readl_relaxed(drv->mpmu + MPMU_CPRR) | MPMU_CPRR_DSPR | MPMU_CPRR_BBR;
	writel_relaxed(reg, drv->mpmu + MPMU_CPRR);
	udelay(10);

	/* Reset and enable WDT 256 Hz clock */
	writel_relaxed(0x7, drv->mpmu + MPMU_WDTPCR);
	readl_relaxed(drv->mpmu + MPMU_WDTPCR);
	writel_relaxed(0x3, drv->mpmu + MPMU_WDTPCR);
	readl_relaxed(drv->mpmu + MPMU_WDTPCR);
}

static void pxa988_wdt_set_match_locked(struct pxa988_wdt *drv, unsigned int secs)
{
	u32 count, match;

	count = readl_relaxed(drv->wdt + TMR_WVR) & 0xffff;
	match = (count + secs * PXA988_WDT_RATE) & 0xffff;

	pxa988_wdt_unlock(drv->wdt);
	writel_relaxed(match, drv->wdt + TMR_WMR);
}

static int pxa988_wdt_start(struct watchdog_device *wdd)
{
	struct pxa988_wdt *drv = watchdog_get_drvdata(wdd);
	unsigned long flags;

	spin_lock_irqsave(&drv->lock, flags);
	pxa988_wdt_unlock(drv->wdt);
	writel_relaxed(0, drv->wdt + TMR_WSR);
	pxa988_wdt_set_match_locked(drv, wdd->timeout);
	pxa988_wdt_unlock(drv->wdt);
	writel_relaxed(TMR_WMER_WE | TMR_WMER_WRIE, drv->wdt + TMR_WMER);
	spin_unlock_irqrestore(&drv->lock, flags);

	return 0;
}

static int pxa988_wdt_stop(struct watchdog_device *wdd)
{
	struct pxa988_wdt *drv = watchdog_get_drvdata(wdd);
	unsigned long flags;

	spin_lock_irqsave(&drv->lock, flags);
	pxa988_wdt_unlock(drv->wdt);
	writel_relaxed(0, drv->wdt + TMR_WMER);
	pxa988_wdt_unlock(drv->wdt);
	writel_relaxed(0, drv->wdt + TMR_WSR);
	spin_unlock_irqrestore(&drv->lock, flags);

	return 0;
}

static int pxa988_wdt_ping(struct watchdog_device *wdd)
{
	struct pxa988_wdt *drv = watchdog_get_drvdata(wdd);
	unsigned long flags;

	spin_lock_irqsave(&drv->lock, flags);
	pxa988_wdt_set_match_locked(drv, wdd->timeout);
	spin_unlock_irqrestore(&drv->lock, flags);

	return 0;
}

static int pxa988_wdt_set_timeout(struct watchdog_device *wdd, unsigned int t)
{
	wdd->timeout = t;
	if (watchdog_hw_running(wdd))
		return pxa988_wdt_ping(wdd);
	return 0;
}

static const struct watchdog_info pxa988_wdt_info = {
	.options = WDIOF_SETTIMEOUT | WDIOF_KEEPALIVEPING | WDIOF_MAGICCLOSE,
	.identity = "PXA988 Watchdog",
};

static const struct watchdog_ops pxa988_wdt_ops = {
	.owner = THIS_MODULE,
	.start = pxa988_wdt_start,
	.stop = pxa988_wdt_stop,
	.ping = pxa988_wdt_ping,
	.set_timeout = pxa988_wdt_set_timeout,
};

static int pxa988_wdt_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pxa988_wdt *drv;
	struct resource *res;
	int ret;

	drv = devm_kzalloc(dev, sizeof(*drv), GFP_KERNEL);
	if (!drv)
		return -ENOMEM;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "wdt");
	drv->wdt = devm_ioremap_resource(dev, res);
	if (IS_ERR(drv->wdt))
		return PTR_ERR(drv->wdt);

	/* MPMU region overlaps clock-controller@d4050000; map without request */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "mpmu");
	if (!res)
		return -EINVAL;
	drv->mpmu = devm_ioremap(dev, res->start, resource_size(res));
	if (!drv->mpmu)
		return -ENOMEM;

	spin_lock_init(&drv->lock);

	drv->wdd.info = &pxa988_wdt_info;
	drv->wdd.ops = &pxa988_wdt_ops;
	drv->wdd.min_timeout = 1;
	drv->wdd.max_timeout = PXA988_WDT_MAX_TO;
	drv->wdd.timeout = PXA988_WDT_DEFAULT_TO;
	drv->wdd.min_hw_heartbeat_ms = 1000;
	drv->wdd.parent = dev;

	watchdog_set_drvdata(&drv->wdd, drv);
	watchdog_init_timeout(&drv->wdd, timeout, dev);
	watchdog_set_nowayout(&drv->wdd, nowayout);
	watchdog_stop_on_reboot(&drv->wdd);
	watchdog_stop_on_unregister(&drv->wdd);

	pxa988_wdt_hw_init(drv);
	pxa988_wdt_start(&drv->wdd);
	set_bit(WDOG_HW_RUNNING, &drv->wdd.status);

	platform_set_drvdata(pdev, drv);

	ret = devm_watchdog_register_device(dev, &drv->wdd);
	if (ret) {
		pxa988_wdt_stop(&drv->wdd);
		return ret;
	}

	dev_info(dev, "PXA988 hardware watchdog enabled (timeout=%us)\n",
		 drv->wdd.timeout);
	return 0;
}

static int __maybe_unused pxa988_wdt_suspend_noirq(struct device *dev)
{
	struct pxa988_wdt *drv = dev_get_drvdata(dev);

	if (watchdog_hw_running(&drv->wdd))
		pxa988_wdt_stop(&drv->wdd);

	return 0;
}

static int __maybe_unused pxa988_wdt_resume_noirq(struct device *dev)
{
	struct pxa988_wdt *drv = dev_get_drvdata(dev);

	if (watchdog_hw_running(&drv->wdd)) {
		pxa988_wdt_hw_init(drv);
		pxa988_wdt_start(&drv->wdd);
	}

	return 0;
}

static const struct dev_pm_ops pxa988_wdt_pm_ops = {
	SET_NOIRQ_SYSTEM_SLEEP_PM_OPS(pxa988_wdt_suspend_noirq,
				      pxa988_wdt_resume_noirq)
};

static const struct of_device_id pxa988_wdt_of_match[] = {
	{ .compatible = "marvell,pxa988-wdt" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, pxa988_wdt_of_match);

static struct platform_driver pxa988_wdt_driver = {
	.driver = {
		.name = "pxa988-wdt",
		.of_match_table = pxa988_wdt_of_match,
		.pm = &pxa988_wdt_pm_ops,
	},
	.probe = pxa988_wdt_probe,
};
module_platform_driver(pxa988_wdt_driver);

MODULE_DESCRIPTION("Marvell PXA988/PXA986 Hardware Watchdog Driver");
MODULE_LICENSE("GPL v2");
