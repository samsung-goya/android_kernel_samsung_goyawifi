// SPDX-License-Identifier: GPL-2.0
/*
 * Marvell PXA988 / PXA986 ChipIdea USB 2.0 High-Speed Controller Glue & PHY
 *
 * Sourced from Samsung vendor kernel (K34 board-goya.c / pxa988.c),
 * Marvell 3.10 kernel (K310 phy-mv-usb2.c), and KML pxa98x-usb.c.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/usb/chipidea.h>

struct ci_hdrc_pxa {
	struct device *dev;
	void __iomem *phy_base;
	void __iomem *clk_reg;
	struct clk *clk;
	struct phy *phy;
	struct platform_device *ci_pdev;
	struct ci_hdrc_platform_data pdata;
	u32 sleep_clock;
	bool clock_sleep;
};

static int pxa988_usb_phy_init(struct phy *phy)
{
	struct ci_hdrc_pxa *pxa = phy_get_drvdata(phy);
	void __iomem *base = pxa->phy_base;
	u16 value;
	int ret;

	/*
	 * Exact Samsung goyawifi PHY calibration values.
	 * Matches K34 board-goya.c usb_phy_init_internal() and KML pxa98x-usb.c.
	 */
	static const struct { u16 offset, value; } regs[] = {
		{0x04, 0x5af0}, /* PLLVDD18=1, REFDIV=13, FBDIV=240 */
		{0x08, 0x3133}, /* PU_PLL, LOCK_BYPASS, ICP=1, KVCO=3, CAL12=3 */
		{0x10, 0x0188}, /* IMPCAL_VTH=1, HS/FS_RCAL=8 */
		{0x14, 0x07d4}, /* TXVDD15=1, TXVDD12=3, LOWVDD, AMP=5, PHSEL=4 */
		{0x18, 0x0aff}, /* SLEWRATE=2, CAL_DLY=2, FS/HS_DRV=15 */
		{0x20, 0xaac1}, /* GOYA SQ_THRESH=12 */
		{0x24, 0x3892},
		{0x28, 0x0105},
		{0x30, 0x0110},
		{0x34, 0x5680},
		{0x3c, 0x2586},
		{0x40, 0xe400},
		{0x58, 0x0000},
	};
	unsigned int i;

	/* Power up PHY power block */
	writel(0x10901003, base + 0x104);

	for (i = 0; i < ARRAY_SIZE(regs); i++)
		writew(regs[i].value, base + regs[i].offset);

	ret = readw_poll_timeout(base + 0x08, value, value & BIT(15),
				 10, 200000);
	dev_info(&phy->dev, "PXA988 USB PHY PLL=%04x power=%08x ready=%d\n",
		 value, readl(base + 0x104), !ret);

	/* Vendor initializes without requiring the PLL_READY status bit */
	return 0;
}

static int pxa988_usb_phy_exit(struct phy *phy)
{
	struct ci_hdrc_pxa *pxa = phy_get_drvdata(phy);
	void __iomem *base = pxa->phy_base;

	/* Power down PLL */
	writew(readw(base + 0x08) & ~BIT(13), base + 0x08);
	/* Power down Analog part */
	writew(readw(base + 0x34) & ~BIT(14), base + 0x34);
	/* Power down PHY block */
	writel(0x10901000, base + 0x104);

	return 0;
}

static const struct phy_ops pxa988_usb_phy_ops = {
	.init = pxa988_usb_phy_init,
	.exit = pxa988_usb_phy_exit,
	.owner = THIS_MODULE,
};

static ssize_t clock_sleep_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct ci_hdrc_pxa *pxa = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", READ_ONCE(pxa->clock_sleep));
}

static ssize_t clock_sleep_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t len)
{
	struct ci_hdrc_pxa *pxa = dev_get_drvdata(dev);
	bool enable;
	int ret = kstrtobool(buf, &enable);

	if (ret)
		return ret;
	WRITE_ONCE(pxa->clock_sleep, enable);
	return len;
}
static DEVICE_ATTR_RW(clock_sleep);

static struct attribute *ci_hdrc_pxa_attrs[] = {
	&dev_attr_clock_sleep.attr,
	NULL,
};
ATTRIBUTE_GROUPS(ci_hdrc_pxa);

static int ci_hdrc_pxa_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ci_hdrc_pxa *pxa;
	struct resource *res_ctrl, *res_phy, *res_clk;
	struct resource ci_res[2];
	int irq, ret;

	pxa = devm_kzalloc(dev, sizeof(*pxa), GFP_KERNEL);
	if (!pxa)
		return -ENOMEM;

	pxa->dev = dev;

	/* Controller registers (0xd4208000, 0x1000) */
	res_ctrl = platform_get_resource_byname(pdev, IORESOURCE_MEM, "controller");
	if (!res_ctrl) {
		/* Fallback to index 0 if not named */
		res_ctrl = platform_get_resource(pdev, IORESOURCE_MEM, 0);
		if (!res_ctrl)
			return -EINVAL;
	}

	/* PHY registers (0xd4207000, 0x200) */
	res_phy = platform_get_resource_byname(pdev, IORESOURCE_MEM, "phy");
	if (!res_phy) {
		/* Fallback to index 1 if not named */
		res_phy = platform_get_resource(pdev, IORESOURCE_MEM, 1);
		if (!res_phy)
			return -EINVAL;
	}

	pxa->phy_base = devm_ioremap_resource(dev, res_phy);
	if (IS_ERR(pxa->phy_base))
		return PTR_ERR(pxa->phy_base);

	/* Optional direct clock MMIO resource (0xd428285c) */
	res_clk = platform_get_resource_byname(pdev, IORESOURCE_MEM, "clock");
	if (res_clk) {
		pxa->clk_reg = devm_ioremap_resource(dev, res_clk);
		if (IS_ERR(pxa->clk_reg))
			return PTR_ERR(pxa->clk_reg);
	}

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		dev_err(dev, "failed to get IRQ: %d\n", irq);
		return irq;
	}

	/* Common clock framework support */
	pxa->clk = devm_clk_get(dev, NULL);
	if (!IS_ERR(pxa->clk)) {
		ret = clk_prepare_enable(pxa->clk);
		if (ret) {
			dev_err(dev, "failed to enable usb clock: %d\n", ret);
			return ret;
		}
	} else if (pxa->clk_reg) {
		/* Direct register clock enable: release AXI reset & enable clock */
		writel(0x9, pxa->clk_reg);
		readl(pxa->clk_reg);
		udelay(20);
	} else {
		dev_warn(dev, "neither clk nor clock MMIO resource provided\n");
	}

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret) {
		dev_err(dev, "failed to set 32-bit DMA mask\n");
		goto err_clk;
	}

	/* Register generic PHY */
	pxa->phy = devm_phy_create(dev, NULL, &pxa988_usb_phy_ops);
	if (IS_ERR(pxa->phy)) {
		ret = PTR_ERR(pxa->phy);
		dev_err(dev, "failed to create generic phy: %d\n", ret);
		goto err_clk;
	}
	phy_set_drvdata(pxa->phy, pxa);

	/* Configure ChipIdea platform data */
	pxa->pdata.name = "ci_hdrc_pxa";
	pxa->pdata.capoffset = DEF_CAPOFFSET;
	pxa->pdata.phy_mode = USBPHY_INTERFACE_MODE_UTMI;
	pxa->pdata.dr_mode = usb_get_dr_mode(dev);
	if (pxa->pdata.dr_mode == USB_DR_MODE_UNKNOWN)
		pxa->pdata.dr_mode = USB_DR_MODE_PERIPHERAL;

	pxa->pdata.flags = CI_HDRC_DISABLE_STREAMING | CI_HDRC_REQUIRES_ALIGNED_DMA;

	if (of_property_read_bool(dev->of_node, "force-vbus"))
		pxa->pdata.flags |= CI_HDRC_FORCE_VBUS_ACTIVE_ALWAYS;

	pxa->pdata.phy = pxa->phy;

	ci_res[0] = *res_ctrl;
	ci_res[1] = (struct resource) {
		.start = irq,
		.end = irq,
		.flags = IORESOURCE_IRQ,
	};

	platform_set_drvdata(pdev, pxa);

	pxa->ci_pdev = ci_hdrc_add_device(dev, ci_res, ARRAY_SIZE(ci_res), &pxa->pdata);
	if (IS_ERR(pxa->ci_pdev)) {
		ret = PTR_ERR(pxa->ci_pdev);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "failed to register ChipIdea HDRC: %d\n", ret);
		goto err_clk;
	}

	dev_info(dev, "PXA988 ChipIdea USB peripheral registered, IRQ %d\n", irq);
	return 0;

err_clk:
	if (!IS_ERR(pxa->clk))
		clk_disable_unprepare(pxa->clk);
	else if (pxa->clk_reg)
		writel(0, pxa->clk_reg);
	return ret;
}

static int ci_hdrc_pxa_remove(struct platform_device *pdev)
{
	struct ci_hdrc_pxa *pxa = platform_get_drvdata(pdev);

	ci_hdrc_remove_device(pxa->ci_pdev);

	if (!IS_ERR(pxa->clk))
		clk_disable_unprepare(pxa->clk);
	else if (pxa->clk_reg)
		writel(0, pxa->clk_reg);

	return 0;
}

static int ci_hdrc_pxa_suspend_late(struct device *dev)
{
	struct ci_hdrc_pxa *pxa = dev_get_drvdata(dev);

	if (pxa->clk_reg)
		pxa->sleep_clock = readl(pxa->clk_reg);

	if (READ_ONCE(pxa->clock_sleep)) {
		if (pxa->clk_reg) {
			writel(pxa->sleep_clock & ~BIT(3), pxa->clk_reg);
			readl(pxa->clk_reg);
		} else if (!IS_ERR(pxa->clk)) {
			clk_disable_unprepare(pxa->clk);
		}
		dev_info(dev, "suspend AXI clock gated, reset retained\n");
	}
	return 0;
}

static int ci_hdrc_pxa_resume_early(struct device *dev)
{
	struct ci_hdrc_pxa *pxa = dev_get_drvdata(dev);

	if (READ_ONCE(pxa->clock_sleep)) {
		if (pxa->clk_reg) {
			writel(pxa->sleep_clock, pxa->clk_reg);
			readl(pxa->clk_reg);
		} else if (!IS_ERR(pxa->clk)) {
			clk_prepare_enable(pxa->clk);
		}
		udelay(20);
	}
	return 0;
}

static const struct dev_pm_ops ci_hdrc_pxa_pm = {
	SET_LATE_SYSTEM_SLEEP_PM_OPS(ci_hdrc_pxa_suspend_late,
				     ci_hdrc_pxa_resume_early)
};

static const struct of_device_id ci_hdrc_pxa_match[] = {
	{ .compatible = "marvell,pxa986-usb" },
	{ .compatible = "marvell,pxa988-usb" },
	{ }
};
MODULE_DEVICE_TABLE(of, ci_hdrc_pxa_match);

static struct platform_driver ci_hdrc_pxa_driver = {
	.probe = ci_hdrc_pxa_probe,
	.remove = ci_hdrc_pxa_remove,
	.driver = {
		.name = "ci-hdrc-pxa",
		.of_match_table = ci_hdrc_pxa_match,
		.pm = &ci_hdrc_pxa_pm,
		.groups = ci_hdrc_pxa_groups,
	},
};
module_platform_driver(ci_hdrc_pxa_driver);

MODULE_AUTHOR("Antigravity / Google DeepMind");
MODULE_DESCRIPTION("Marvell PXA988/PXA986 ChipIdea USB Glue & PHY Driver");
MODULE_LICENSE("GPL v2");
