// SPDX-License-Identifier: GPL-2.0
/*
 * Marvell PXA988 / PXA986 (Cortex-A9) Device Tree machine descriptor
 *
 * Copyright (C) 2012 Marvell International Ltd.
 */

#include <linux/clk-provider.h>
#include <linux/clocksource.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/irqchip.h>
#include <linux/of_platform.h>
#include <linux/reboot.h>
#include <asm/mach/arch.h>
#include <asm/mach/time.h>
#include <asm/system_misc.h>

#include "addr-map.h"
#include "common.h"
#include "regs-apbc.h"

/*
 * PXA988 legacy ICU global IRQ/FIQ mask registers.
 *
 * In active mode, peripheral interrupts go directly to the Cortex-A9 GIC
 * SPI inputs. The legacy ICU CPU0/CPU1 global IRQ/FIQ outputs must be
 * masked so the ICU does not assert the legacy nIRQ/nFIQ lines alongside
 * the GIC.
 */
#define PXA988_ICU_VIRT_BASE		(AXI_VIRT_BASE + 0x82000)
#define PXA988_ICU_A9C0_GBL_INT_MSK	(PXA988_ICU_VIRT_BASE + 0x114)
#define PXA988_ICU_A9C1_GBL_INT_MSK	(PXA988_ICU_VIRT_BASE + 0x144)
#define PXA988_ICU_GBL_IRQ_FIQ_MSK	0x3

/* APBC timers0 clock control register (0xd4015034) */
#define APBC_TIMERS0			APBC_REG(0x0034)

static void __init pxa988_dt_init_irq(void)
{
	/* Mask legacy ICU global IRQ/FIQ outputs to CPU0 and CPU1 */
	writel_relaxed(PXA988_ICU_GBL_IRQ_FIQ_MSK, PXA988_ICU_A9C0_GBL_INT_MSK);
	writel_relaxed(PXA988_ICU_GBL_IRQ_FIQ_MSK, PXA988_ICU_A9C1_GBL_INT_MSK);

	irqchip_init();
}

static void __init pxa988_dt_init_time(void)
{
	/*
	 * Enable APBC timers0 functional and bus clock at 3.25 MHz
	 * (FNCLKSEL = 3) and release reset before initializing timer0.
	 */
	writel_relaxed(APBC_APBCLK | APBC_RST, APBC_TIMERS0);
	writel_relaxed(APBC_APBCLK | APBC_FNCLK | APBC_FNCLKSEL(3),
		       APBC_TIMERS0);

	of_clk_init(NULL);
	mmp_dt_init_timer();
	timer_probe();
}

static void pxa988_restart(enum reboot_mode mode, const char *cmd)
{
	void __iomem *mpmu = ioremap(0xd4050000, 0x2000);
	void __iomem *wdt = ioremap(0xd4080000, 0x100);

	if (cmd && !strcmp(cmd, "recovery")) {
		void __iomem *rtc_br0 = ioremap(0xd4010014, 4);
		if (rtc_br0) {
			writel_relaxed(0x7265636f, rtc_br0);
			iounmap(rtc_br0);
		}
	}

	if (mpmu && wdt) {
		u32 reg, count;

		/* Hold CP and reset MSA to avoid watchdog reset cancellation */
		reg = readl_relaxed(mpmu + 0x1020) | BIT(0); /* MPMU_APRR_CPR */
		writel_relaxed(reg, mpmu + 0x1020);
		udelay(10);
		reg = readl_relaxed(mpmu + 0x0020) | BIT(2) | BIT(3); /* MPMU_CPRR DSPR | BBR */
		writel_relaxed(reg, mpmu + 0x0020);
		udelay(10);

		/* Reset and enable WDT clock */
		writel_relaxed(0x7, mpmu + 0x0200); /* MPMU_WDTPCR */
		readl_relaxed(mpmu + 0x0200);
		writel_relaxed(0x3, mpmu + 0x0200);
		readl_relaxed(mpmu + 0x0200);

		/* Enable WDT reset */
		writel_relaxed(0xbaba, wdt + 0x9c); /* TMR_WFAR */
		writel_relaxed(0xeb10, wdt + 0xa0); /* TMR_WSAR */
		writel_relaxed(0x3, wdt + 0x64);    /* TMR_WMER */

		/* Negate HW reset to WDT after system reset */
		reg = readl_relaxed(mpmu + 0x1020) | BIT(4); /* MPMU_APRR_WDTR */
		writel_relaxed(reg, mpmu + 0x1020);

		/* Clear previous WDT status */
		writel_relaxed(0xbaba, wdt + 0x9c);
		writel_relaxed(0xeb10, wdt + 0xa0);
		writel_relaxed(0, wdt + 0x70);      /* TMR_WSR */

		count = readl_relaxed(wdt + 0x6c);  /* TMR_WVR */

		/* Set match counter for immediate reset */
		writel_relaxed(0xbaba, wdt + 0x9c);
		writel_relaxed(0xeb10, wdt + 0xa0);
		writel_relaxed((count + 0x20) & 0xffff, wdt + 0x68); /* TMR_WMR */
	}

	mdelay(500);
	soft_restart(0);
}

static const char *const pxa988_dt_board_compat[] __initconst = {
	"marvell,pxa988",
	"marvell,pxa986",
	NULL,
};

DT_MACHINE_START(PXA988_DT, "Marvell PXA988 (Device Tree)")
	.map_io		= mmp_map_io,
	.init_irq	= pxa988_dt_init_irq,
	.init_time	= pxa988_dt_init_time,
	.restart	= pxa988_restart,
	.dt_compat	= pxa988_dt_board_compat,
	.l2c_aux_val	= 0x30800000,
	.l2c_aux_mask	= 0xce7fffff,
MACHINE_END
