// SPDX-License-Identifier: GPL-2.0
/*
 * Marvell PXA988 / PXA986 Device Tree Clock Framework
 *
 * Copyright (C) 2012-2014 Marvell Technology Group Ltd.
 * Copyright (C) 2026 The LineageOS Project / postmarketOS contributors
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/spinlock.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/slab.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>

#include <dt-bindings/clock/marvell,pxa988.h>

#include "clk.h"
#include "reset.h"

/* MPMU register offsets */
#define MPMU_FCCR		0x0008
#define MPMU_POSR		0x0010
#define MPMU_UART_PLL		0x0014
#define MPMU_PLL3CR		0x001c
#define MPMU_PLL2CR		0x0034
#define MPMU_VRCR		0x0064

/* APMU register offsets */
#define APMU_CCR		0x0004
#define APMU_CCSR		0x000c
#define APMU_SQU_CLK_GATE	0x001c
#define APMU_CLK_GATE_CTRL	0x0040
#define APMU_DSI1		0x0044
#define APMU_DISP1		0x004c
#define APMU_CCIC0		0x0050
#define APMU_SDH0		0x0054
#define APMU_SDH1		0x0058
#define APMU_USB		0x005c
#define APMU_DFC		0x0060
#define APMU_GEU		0x0068
#define APMU_TRACE		0x0088
#define APMU_VPU		0x00a4
#define APMU_PLL_SEL_STATUS	0x00c4
#define APMU_GC			0x00cc
#define APMU_GC2D		0x00d4
#define APMU_SDH2		0x00e0
#define APMU_MCK4_CTL		0x00e8

/* APBC register offsets */
#define APBC_UART0		0x0000
#define APBC_UART1		0x0004
#define APBC_GPIO		0x0008
#define APBC_PWM0		0x000c
#define APBC_PWM1		0x0010
#define APBC_PWM2		0x0014
#define APBC_PWM3		0x0018
#define APBC_SSP0		0x001c
#define APBC_SSP1		0x0020
#define APBC_IPC		0x0024
#define APBC_RTC		0x0028
#define APBC_TWSI0		0x002c
#define APBC_KPC		0x0030
#define APBC_TIMER0		0x0034
#define APBC_TIMER1		0x0038
#define APBC_SSP2		0x004c
#define APBC_THERMAL		0x0050
#define APBC_TWSI1		0x0060

/* APBCP register offsets */
#define APBCP_UART2		0x001c
#define APBCP_TWSI2		0x0028
#define APBCP_RIPC		0x0038

/* APBS register offsets */
#define APBS_PLL2CR		0x0104
#define APBS_PLL3CR		0x0108

/* CIU register offsets */
#define CIU_MC_CONF		0x0040
#define CIU_GPU2D_XTC		0x00a0
#define CIU_GPU_XTC		0x00a4

/* SDH bits */
#define SDH_ALL_RESET		BIT(0)
#define SDH_ACLK_EN		BIT(3)

struct pxa988_clk_unit {
	struct mmp_clk_unit unit;
	void __iomem *mpmu_base;
	void __iomem *apmu_base;
	void __iomem *apbc_base;
	void __iomem *apbcp_base;
	void __iomem *apbs_base;
	void __iomem *ciu_base;
};

static struct mmp_param_fixed_rate_clk fixed_rate_clks[] = {
	{PXA988_CLK_CLK32, "clk32", NULL, 0, 32768},
	{PXA988_CLK_VCTCXO, "vctcxo", NULL, 0, 26000000},
	{PXA988_CLK_PLL1_416, "pll1_416", NULL, 0, 416000000},
	{PXA988_CLK_PLL1_624, "pll1_624", NULL, 0, 624000000},
	{PXA988_CLK_PLL1_1248, "pll1_1248", NULL, 0, 1248000000},
	{PXA988_CLK_USB_PLL, "usb_pll", NULL, 0, 480000000},
};

static struct mmp_param_fixed_factor_clk fixed_factor_clks[] = {
	{PXA988_CLK_PLL1_2, "pll1_2", "pll1_624", 1, 2, 0},
	{PXA988_CLK_PLL1_4, "pll1_4", "pll1_2", 1, 2, 0},
	{PXA988_CLK_PLL1_8, "pll1_8", "pll1_4", 1, 2, 0},
	{PXA988_CLK_PLL1_16, "pll1_16", "pll1_8", 1, 2, 0},
	{PXA988_CLK_PLL1_6, "pll1_6", "pll1_2", 1, 3, 0},
	{PXA988_CLK_PLL1_12, "pll1_12", "pll1_6", 1, 2, 0},
	{PXA988_CLK_PLL1_24, "pll1_24", "pll1_12", 1, 2, 0},
	{PXA988_CLK_PLL1_48, "pll1_48", "pll1_24", 1, 2, 0},
	{PXA988_CLK_PLL1_96, "pll1_96", "pll1_48", 1, 2, 0},
	{PXA988_CLK_PLL1_192, "pll1_192", "pll1_96", 1, 2, 0},
	{PXA988_CLK_PLL1_13, "pll1_13", "pll1_624", 1, 13, 0},
	{PXA988_CLK_PLL1_13_1_5, "pll1_13_1_5", "pll1_13", 2, 3, 0},
	{PXA988_CLK_PLL1_2_1_5, "pll1_2_1_5", "pll1_2", 2, 3, 0},
	{PXA988_CLK_PLL1_3_16, "pll1_3_16", "pll1_624", 3, 16, 0},
};

static struct mmp_clk_factor_masks uart_factor_masks = {
	.factor = 2,
	.num_mask = 0x1fff,
	.den_mask = 0x1fff,
	.num_shift = 16,
	.den_shift = 0,
};

static struct mmp_clk_factor_tbl uart_factor_tbl[] = {
	{.num = 8125, .den = 1536},	/* 14.7456 MHz from pll1_4 */
};

static DEFINE_SPINLOCK(pll1_gate_lock);

static u32 pxa988_pll_divsel_to_div(u32 divsel)
{
	switch (divsel) {
	case 0:
		return 1;
	case 2:
		return 2;
	case 4:
		return 3;
	case 5:
		return 4;
	case 7:
		return 6;
	case 8:
		return 8;
	default:
		return 1;
	}
}

static void __init pxa988_pll_init(struct pxa988_clk_unit *pxa_unit)
{
	struct mmp_clk_unit *unit = &pxa_unit->unit;
	struct clk *clk;
	u32 pll2cr, pll3cr, apbs2, apbs3;
	u32 refd, fbd, divse, divdiff;
	unsigned long pll2_vco = 1600000000UL, pll2 = 800000000UL, pll2p = 800000000UL;
	unsigned long pll3_vco = 2410000000UL, pll3 = 803333333UL, pll3p = 1205000000UL;

	mmp_register_fixed_rate_clks(unit, fixed_rate_clks,
				     ARRAY_SIZE(fixed_rate_clks));

	mmp_register_fixed_factor_clks(unit, fixed_factor_clks,
				       ARRAY_SIZE(fixed_factor_clks));

	/* PLL1 gated outputs with D1P sleep feature */
	clk = clk_register_gate(NULL, "pll1_416_gate", "pll1_416",
				CLK_SET_RATE_PARENT | CLK_IGNORE_UNUSED,
				pxa_unit->apmu_base + APMU_CLK_GATE_CTRL,
				27, 0, &pll1_gate_lock);
	mmp_clk_add(unit, PXA988_CLK_PLL1_416_GATE, clk);

	clk = clk_register_gate(NULL, "pll1_624_gate", "pll1_624",
				CLK_SET_RATE_PARENT | CLK_IGNORE_UNUSED,
				pxa_unit->apmu_base + APMU_CLK_GATE_CTRL,
				26, 0, &pll1_gate_lock);
	mmp_clk_add(unit, PXA988_CLK_PLL1_624_GATE, clk);

	/* UART PLL generator */
	clk = mmp_clk_register_factor("uart_pll", "pll1_4",
				      CLK_SET_RATE_PARENT,
				      pxa_unit->mpmu_base + MPMU_UART_PLL,
				      &uart_factor_masks, uart_factor_tbl,
				      ARRAY_SIZE(uart_factor_tbl), NULL);
	mmp_clk_add(unit, PXA988_CLK_UART_PLL, clk);

	/* Read live boot rates for PLL2 */
	pll2cr = readl_relaxed(pxa_unit->mpmu_base + MPMU_PLL2CR);
	if (!(pll2cr & BIT(9)) || (pll2cr & BIT(8))) {
		refd = (pll2cr >> 19) & 0x1f;
		if (refd == 0)
			refd = 1;
		fbd = (pll2cr >> 10) & 0x1ff;
		pll2_vco = DIV_ROUND_UP_ULL(26000000ULL * fbd, refd);

		apbs2 = readl_relaxed(pxa_unit->apbs_base + APBS_PLL2CR);
		divse = pxa988_pll_divsel_to_div((apbs2 >> 4) & 0xf);
		divdiff = pxa988_pll_divsel_to_div((apbs2 >> 8) & 0xf);
		pll2 = pll2_vco / divse;
		pll2p = pll2_vco / divdiff;
	}

	/* Read live boot rates for PLL3 */
	pll3cr = readl_relaxed(pxa_unit->mpmu_base + MPMU_PLL3CR);
	if ((pll3cr & BIT(19)) || (pll3cr & BIT(18))) {
		refd = pll3cr & 0x1f;
		if (refd == 0)
			refd = 1;
		fbd = (pll3cr >> 5) & 0x1ff;
		pll3_vco = DIV_ROUND_UP_ULL(26000000ULL * fbd, refd);

		apbs3 = readl_relaxed(pxa_unit->apbs_base + APBS_PLL3CR);
		divse = pxa988_pll_divsel_to_div((apbs3 >> 4) & 0xf);
		divdiff = pxa988_pll_divsel_to_div((apbs3 >> 8) & 0xf);
		pll3 = pll3_vco / divse;
		pll3p = pll3_vco / divdiff;
	}

	pr_info("pxa988-clk: PLL2 VCO %lu MHz, PLL2 %lu MHz, PLL2P %lu MHz\n",
		pll2_vco / 1000000, pll2 / 1000000, pll2p / 1000000);
	pr_info("pxa988-clk: PLL3 VCO %lu MHz, PLL3 %lu MHz, PLL3P %lu MHz\n",
		pll3_vco / 1000000, pll3 / 1000000, pll3p / 1000000);

	clk = clk_register_fixed_rate(NULL, "pll2_vco", NULL, 0, pll2_vco);
	mmp_clk_add(unit, PXA988_CLK_PLL2_VCO, clk);

	clk = clk_register_fixed_rate(NULL, "pll2", "pll2_vco", 0, pll2);
	mmp_clk_add(unit, PXA988_CLK_PLL2, clk);

	clk = clk_register_fixed_rate(NULL, "pll2p", "pll2_vco", 0, pll2p);
	mmp_clk_add(unit, PXA988_CLK_PLL2P, clk);

	clk = clk_register_fixed_rate(NULL, "pll3_vco", NULL, 0, pll3_vco);
	mmp_clk_add(unit, PXA988_CLK_PLL3_VCO, clk);

	clk = clk_register_fixed_rate(NULL, "pll3", "pll3_vco", 0, pll3);
	mmp_clk_add(unit, PXA988_CLK_PLL3, clk);

	clk = clk_register_fixed_rate(NULL, "pll3p", "pll3_vco", 0, pll3p);
	mmp_clk_add(unit, PXA988_CLK_PLL3P, clk);
}

static void __init pxa988_core_clk_init(struct pxa988_clk_unit *pxa_unit)
{
	struct mmp_clk_unit *unit = &pxa_unit->unit;
	struct clk *clk;
	u32 pllsel, ccsr, pll3cr;
	u32 apclksel, ddrclksel, axiclksel;
	u32 core_div, ddr_div, bus_div;
	const char *cpu_parent = "pll3p";
	const char *ddr_parent = "pll1_416";
	const char *axi_parent = "pll1_416";

	pllsel = readl_relaxed(pxa_unit->apmu_base + APMU_PLL_SEL_STATUS);
	ccsr = readl_relaxed(pxa_unit->apmu_base + APMU_CCSR);
	/* Pulse core_rd_st_clear so hardware FC lock is released */
	writel_relaxed(readl_relaxed(pxa_unit->apmu_base + APMU_CCR) | BIT(31),
		       pxa_unit->apmu_base + APMU_CCR);

	pll3cr = readl_relaxed(pxa_unit->mpmu_base + MPMU_PLL3CR);

	apclksel = (pllsel >> 2) & 0x3;
	ddrclksel = (pllsel >> 4) & 0x3;
	axiclksel = (pllsel >> 6) & 0x3;

	switch (apclksel) {
	case 0:
		cpu_parent = "pll1_624";
		break;
	case 1:
		cpu_parent = (pll3cr & BIT(18)) ? "pll3p" : "pll1_1248";
		break;
	case 2:
		cpu_parent = "pll2";
		break;
	case 3:
		cpu_parent = "pll2p";
		break;
	}

	switch (ddrclksel) {
	case 0:
		ddr_parent = "pll1_416";
		break;
	case 1:
		ddr_parent = "pll1_624";
		break;
	case 2:
		ddr_parent = "pll2";
		break;
	case 3:
		ddr_parent = "pll2p";
		break;
	}

	switch (axiclksel) {
	case 0:
		axi_parent = "pll1_416";
		break;
	case 1:
		axi_parent = "pll1_624";
		break;
	case 2:
		axi_parent = "pll2";
		break;
	case 3:
		axi_parent = "pll2p";
		break;
	}

	core_div = (ccsr & 0x7) + 1;
	ddr_div = (((ccsr >> 12) & 0x7) + 1) * 2;
	bus_div = (((ccsr >> 15) & 0x7) + 1);

	clk = clk_register_fixed_factor(NULL, "cpu", cpu_parent,
					CLK_SET_RATE_PARENT, 1, core_div);
	mmp_clk_add(unit, PXA988_CLK_CPU, clk);

	/* TWD is hardwired as CPU clock / 8 */
	clk = clk_register_fixed_factor(NULL, "twd", "cpu", 0, 1, 8);
	mmp_clk_add(unit, PXA988_CLK_TWD, clk);

	clk = clk_register_fixed_factor(NULL, "ddr", ddr_parent,
					CLK_SET_RATE_PARENT, 1, ddr_div);
	mmp_clk_add(unit, PXA988_CLK_DDR, clk);

	clk = clk_register_fixed_factor(NULL, "axi", axi_parent,
					CLK_SET_RATE_PARENT, 1, bus_div);
	mmp_clk_add(unit, PXA988_CLK_AXI, clk);

	pr_info("pxa988-clk: CPU %lu MHz (from %s div %u), TWD %lu MHz\n",
		clk_get_rate(unit->clk_table[PXA988_CLK_CPU]) / 1000000,
		cpu_parent, core_div,
		clk_get_rate(unit->clk_table[PXA988_CLK_TWD]) / 1000000);
	pr_info("pxa988-clk: DDR %lu MHz (from %s div %u), AXI %lu MHz (from %s div %u)\n",
		clk_get_rate(unit->clk_table[PXA988_CLK_DDR]) / 1000000,
		ddr_parent, ddr_div,
		clk_get_rate(unit->clk_table[PXA988_CLK_AXI]) / 1000000,
		axi_parent, bus_div);
}

static DEFINE_SPINLOCK(uart0_lock);
static DEFINE_SPINLOCK(uart1_lock);
static DEFINE_SPINLOCK(uart2_lock);
static const char * const uart_parent_names[] = {"pll1_3_16", "uart_pll"};

static DEFINE_SPINLOCK(ssp0_lock);
static DEFINE_SPINLOCK(ssp1_lock);
static DEFINE_SPINLOCK(ssp2_lock);
static const char * const ssp_parent_names[] = {"pll1_96", "pll1_48", "pll1_24", "pll1_12"};

static DEFINE_SPINLOCK(timer0_lock);
static DEFINE_SPINLOCK(timer1_lock);
static const char * const timer_parent_names[] = {"pll1_48", "clk32", "pll1_96"};

static DEFINE_SPINLOCK(reset_lock);

static struct mmp_param_mux_clk apbc_mux_clks[] = {
	{0, "uart0_mux", (const char **)uart_parent_names, ARRAY_SIZE(uart_parent_names),
	 CLK_SET_RATE_PARENT, APBC_UART0, 4, 3, 0, &uart0_lock},
	{0, "uart1_mux", (const char **)uart_parent_names, ARRAY_SIZE(uart_parent_names),
	 CLK_SET_RATE_PARENT, APBC_UART1, 4, 3, 0, &uart1_lock},
	{0, "ssp0_mux", (const char **)ssp_parent_names, ARRAY_SIZE(ssp_parent_names),
	 CLK_SET_RATE_PARENT, APBC_SSP0, 4, 3, 0, &ssp0_lock},
	{0, "ssp1_mux", (const char **)ssp_parent_names, ARRAY_SIZE(ssp_parent_names),
	 CLK_SET_RATE_PARENT, APBC_SSP1, 4, 3, 0, &ssp1_lock},
	{0, "ssp2_mux", (const char **)ssp_parent_names, ARRAY_SIZE(ssp_parent_names),
	 CLK_SET_RATE_PARENT, APBC_SSP2, 4, 3, 0, &ssp2_lock},
	{0, "timer0_mux", (const char **)timer_parent_names, ARRAY_SIZE(timer_parent_names),
	 CLK_SET_RATE_PARENT, APBC_TIMER0, 4, 3, 0, &timer0_lock},
	{0, "timer1_mux", (const char **)timer_parent_names, ARRAY_SIZE(timer_parent_names),
	 CLK_SET_RATE_PARENT, APBC_TIMER1, 4, 3, 0, &timer1_lock},
};

static struct mmp_param_mux_clk apbcp_mux_clks[] = {
	{0, "uart2_mux", (const char **)uart_parent_names, ARRAY_SIZE(uart_parent_names),
	 CLK_SET_RATE_PARENT, APBCP_UART2, 4, 3, 0, &uart2_lock},
};

static const struct clk_div_table clk_twsi_ref_table[] = {
	{ .val = 0, .div = 19 },	/* 32.8 MHz from pll1_624 */
	{ .val = 1, .div = 12 },	/* 52 MHz from pll1_624 */
	{ .val = 2, .div = 10 },	/* 62.4 MHz from pll1_624 */
	{ .val = 0, .div = 0 },
};

static struct mmp_param_gate_clk apbc_gate_clks[] = {
	{PXA988_CLK_GPIO, "gpio_clk", "vctcxo",
	 CLK_SET_RATE_PARENT | CLK_IGNORE_UNUSED, APBC_GPIO, 0x3, 0x3, 0x0, 0, &reset_lock},
	{PXA988_CLK_KPC, "kpc_clk", "clk32",
	 CLK_SET_RATE_PARENT, APBC_KPC, 0x3, 0x3, 0x0, MMP_CLK_GATE_NEED_DELAY, NULL},
	{PXA988_CLK_RTC, "rtc_clk", "clk32",
	 CLK_SET_RATE_PARENT, APBC_RTC, 0x83, 0x83, 0x0, MMP_CLK_GATE_NEED_DELAY, NULL},
	{PXA988_CLK_PWM0, "pwm0_clk", "pll1_48",
	 CLK_SET_RATE_PARENT, APBC_PWM0, 0x3, 0x3, 0x0, 0, &reset_lock},
	{PXA988_CLK_PWM1, "pwm1_clk", "pll1_48",
	 CLK_SET_RATE_PARENT, APBC_PWM1, 0x3, 0x3, 0x0, 0, &reset_lock},
	{PXA988_CLK_PWM2, "pwm2_clk", "pll1_48",
	 CLK_SET_RATE_PARENT, APBC_PWM2, 0x3, 0x3, 0x0, 0, &reset_lock},
	{PXA988_CLK_PWM3, "pwm3_clk", "pll1_48",
	 CLK_SET_RATE_PARENT, APBC_PWM3, 0x3, 0x3, 0x0, 0, &reset_lock},
	{PXA988_CLK_THERMAL, "thermal_clk", "vctcxo",
	 CLK_SET_RATE_PARENT, APBC_THERMAL, 0x3, 0x3, 0x0, 0, &reset_lock},
	{PXA988_CLK_IPC, "ipc_clk", "vctcxo",
	 CLK_SET_RATE_PARENT | CLK_IGNORE_UNUSED, APBC_IPC, 0x3, 0x3, 0x0, 0, &reset_lock},

	/* Muxed APBC peripheral gates */
	{PXA988_CLK_UART0, "uart0_clk", "uart0_mux",
	 CLK_SET_RATE_PARENT | CLK_IGNORE_UNUSED, APBC_UART0, 0x3, 0x3, 0x0, 0, &uart0_lock},
	{PXA988_CLK_UART1, "uart1_clk", "uart1_mux",
	 CLK_SET_RATE_PARENT | CLK_IGNORE_UNUSED, APBC_UART1, 0x3, 0x3, 0x0, 0, &uart1_lock},
	{PXA988_CLK_SSP0, "ssp0_clk", "ssp0_mux",
	 CLK_SET_RATE_PARENT, APBC_SSP0, 0x3, 0x3, 0x0, 0, &ssp0_lock},
	{PXA988_CLK_SSP1, "ssp1_clk", "ssp1_mux",
	 CLK_SET_RATE_PARENT, APBC_SSP1, 0x3, 0x3, 0x0, 0, &ssp1_lock},
	{PXA988_CLK_TIMER0, "timer0_clk", "timer0_mux",
	 CLK_SET_RATE_PARENT | CLK_IGNORE_UNUSED, APBC_TIMER0, 0x3, 0x3, 0x0, 0, &timer0_lock},
	{PXA988_CLK_TIMER1, "timer1_clk", "timer1_mux",
	 CLK_SET_RATE_PARENT | CLK_IGNORE_UNUSED, APBC_TIMER1, 0x3, 0x3, 0x0, 0, &timer1_lock},
};

static struct mmp_param_gate_clk apbcp_gate_clks[] = {
	{PXA988_CLK_RIPC, "ripc_clk", "vctcxo",
	 CLK_SET_RATE_PARENT | CLK_IGNORE_UNUSED, APBCP_RIPC, 0x3, 0x3, 0x0, 0, &reset_lock},
	{PXA988_CLK_UART2, "uart2_clk", "uart2_mux",
	 CLK_SET_RATE_PARENT | CLK_IGNORE_UNUSED, APBCP_UART2, 0x3, 0x3, 0x0, 0, &uart2_lock},
};

static void __init pxa988_apb_periph_clk_init(struct pxa988_clk_unit *pxa_unit)
{
	struct mmp_clk_unit *unit = &pxa_unit->unit;
	struct clk *clk;

	mmp_register_mux_clks(unit, apbc_mux_clks, pxa_unit->apbc_base,
			      ARRAY_SIZE(apbc_mux_clks));

	mmp_register_mux_clks(unit, apbcp_mux_clks, pxa_unit->apbcp_base,
			      ARRAY_SIZE(apbcp_mux_clks));

	mmp_register_gate_clks(unit, apbc_gate_clks, pxa_unit->apbc_base,
			       ARRAY_SIZE(apbc_gate_clks));

	mmp_register_gate_clks(unit, apbcp_gate_clks, pxa_unit->apbcp_base,
			       ARRAY_SIZE(apbcp_gate_clks));

	/* TWSI clocks use specialized divider table from pll1_624 */
	clk = clk_register_divider_table(NULL, "twsi0_div", "pll1_624",
					 CLK_SET_RATE_PARENT,
					 pxa_unit->apbc_base + APBC_TWSI0,
					 4, 3, 0, clk_twsi_ref_table, &reset_lock);
	clk = mmp_clk_register_apbc("twsi0_clk", "twsi0_div",
				    pxa_unit->apbc_base + APBC_TWSI0, 10, 0, &reset_lock);
	mmp_clk_add(unit, PXA988_CLK_TWSI0, clk);

	clk = clk_register_divider_table(NULL, "twsi1_div", "pll1_624",
					 CLK_SET_RATE_PARENT,
					 pxa_unit->apbc_base + APBC_TWSI1,
					 4, 3, 0, clk_twsi_ref_table, &reset_lock);
	clk = mmp_clk_register_apbc("twsi1_clk", "twsi1_div",
				    pxa_unit->apbc_base + APBC_TWSI1, 10, 0, &reset_lock);
	mmp_clk_add(unit, PXA988_CLK_TWSI1, clk);

	clk = clk_register_divider_table(NULL, "twsi2_div", "pll1_624",
					 CLK_SET_RATE_PARENT,
					 pxa_unit->apbcp_base + APBCP_TWSI2,
					 3, 2, 0, clk_twsi_ref_table, &reset_lock);
	clk = mmp_clk_register_apbc("twsi2_clk", "twsi2_div",
				    pxa_unit->apbcp_base + APBCP_TWSI2, 10, 0, &reset_lock);
	mmp_clk_add(unit, PXA988_CLK_TWSI2, clk);
}

static DEFINE_SPINLOCK(sdh0_lock);
static DEFINE_SPINLOCK(sdh1_lock);
static DEFINE_SPINLOCK(sdh2_lock);
static const char * const sdh_parent_names[] = {"pll1_416", "pll1_624"};

static DEFINE_SPINLOCK(usb_lock);
static DEFINE_SPINLOCK(disp_lock);
static DEFINE_SPINLOCK(ccic0_lock);

static struct mmp_clk_mix_config sdh0_mix_config = {
	.reg_info = DEFINE_MIX_REG_INFO(3, 8, 1, 6, 11),
};

static struct mmp_clk_mix_config sdh1_mix_config = {
	.reg_info = DEFINE_MIX_REG_INFO(3, 8, 1, 6, 11),
};

static struct mmp_clk_mix_config sdh2_mix_config = {
	.reg_info = DEFINE_MIX_REG_INFO(3, 8, 1, 6, 11),
};

static struct mmp_param_gate_clk apmu_gate_clks[] = {
	{PXA988_CLK_DFC, "dfc_clk", "pll1_4",
	 CLK_SET_RATE_PARENT, APMU_DFC, 0x19b, 0x19b, 0x0, 0, NULL},
	{PXA988_CLK_SDH_AXI, "sdh_axi", "axi",
	 CLK_SET_RATE_PARENT | CLK_IGNORE_UNUSED, APMU_SDH0,
	 SDH_ACLK_EN, SDH_ACLK_EN, 0x0, 0, &sdh0_lock},
	{PXA988_CLK_USB, "usb_clk", "usb_pll",
	 0, APMU_USB, 0x9, 0x9, 0x0, 0, &usb_lock},

	/* Display clocks: must keep CLK_IGNORE_UNUSED so simple-framebuffer survives */
	{PXA988_CLK_LCD_HCLK, "lcd_hclk", "axi",
	 CLK_SET_RATE_PARENT | CLK_IGNORE_UNUSED, APMU_DISP1, 0x24, 0x24, 0x0, 0, &disp_lock},
	{PXA988_CLK_LCD_AXI, "lcd_axi", "axi",
	 CLK_SET_RATE_PARENT | CLK_IGNORE_UNUSED, APMU_DISP1, 0x8, 0x8, 0x0, 0, &disp_lock},
	{PXA988_CLK_LCD, "lcd_clk", "pll1_624",
	 CLK_SET_RATE_PARENT | CLK_IGNORE_UNUSED, APMU_DISP1, 0x12, 0x12, 0x0, 0, &disp_lock},
	{PXA988_CLK_DSI_PHY, "dsi_phy_clk", "pll1_624",
	 CLK_SET_RATE_PARENT | CLK_IGNORE_UNUSED, APMU_DSI1, 0x24, 0x24, 0x0, 0, &disp_lock},
	{PXA988_CLK_DSI_ESC, "dsi_esc_clk", "vctcxo",
	 CLK_SET_RATE_PARENT | CLK_IGNORE_UNUSED, APMU_DSI1, 0x18, 0x18, 0x0, 0, &disp_lock},

	/* CCIC camera clocks */
	{PXA988_CLK_CCIC_AXI, "ccic_axi", "axi",
	 CLK_SET_RATE_PARENT, APMU_CCIC0, 0x9, 0x9, 0x0, 0, &ccic0_lock},
	{PXA988_CLK_CCIC0, "ccic0_clk", "pll1_624",
	 CLK_SET_RATE_PARENT, APMU_CCIC0, 0x12, 0x12, 0x0, 0, &ccic0_lock},
	{PXA988_CLK_CCIC0_PHY, "ccic0_phy_clk", "pll1_6",
	 CLK_SET_RATE_PARENT, APMU_CCIC0, 0x24, 0x24, 0x0, 0, &ccic0_lock},

	/* Graphics and video engine clocks */
	{PXA988_CLK_VPU_AXI, "vpu_axi", "axi",
	 CLK_SET_RATE_PARENT, APMU_VPU, 0x8, 0x8, 0x0, 0, NULL},
	{PXA988_CLK_VPU, "vpu_clk", "pll1_416",
	 CLK_SET_RATE_PARENT, APMU_VPU, 0x30, 0x30, 0x0, 0, NULL},
	{PXA988_CLK_GC_AXI, "gc_axi", "axi",
	 CLK_SET_RATE_PARENT, APMU_GC, 0x8, 0x8, 0x0, 0, NULL},
	{PXA988_CLK_GC, "gc_clk", "pll1_416",
	 CLK_SET_RATE_PARENT, APMU_GC, 0x30, 0x30, 0x0, 0, NULL},
	{PXA988_CLK_GC2D, "gc2d_clk", "pll1_416",
	 CLK_SET_RATE_PARENT, APMU_GC2D, 0x30, 0x30, 0x0, 0, NULL},
};

static void __init pxa988_axi_periph_clk_init(struct pxa988_clk_unit *pxa_unit)
{
	struct mmp_clk_unit *unit = &pxa_unit->unit;
	struct clk *clk;

	/* Register SDH0 (SD card) mix clock and functional gate */
	sdh0_mix_config.reg_info.reg_clk_ctrl = pxa_unit->apmu_base + APMU_SDH0;
	clk = mmp_clk_register_mix(NULL, "sdh0_mix_clk", (const char **)sdh_parent_names,
				   ARRAY_SIZE(sdh_parent_names),
				   CLK_SET_RATE_PARENT,
				   &sdh0_mix_config, &sdh0_lock);
	clk = mmp_clk_register_gate(NULL, "sdh0_clk", "sdh0_mix_clk",
				    CLK_SET_RATE_PARENT,
				    pxa_unit->apmu_base + APMU_SDH0,
				    0x12, 0x12, 0x0, 0, &sdh0_lock);
	mmp_clk_add(unit, PXA988_CLK_SDH0, clk);

	/* Register SDH1 (WiFi) mix clock and functional gate */
	sdh1_mix_config.reg_info.reg_clk_ctrl = pxa_unit->apmu_base + APMU_SDH1;
	clk = mmp_clk_register_mix(NULL, "sdh1_mix_clk", (const char **)sdh_parent_names,
				   ARRAY_SIZE(sdh_parent_names),
				   CLK_SET_RATE_PARENT,
				   &sdh1_mix_config, &sdh1_lock);
	clk = mmp_clk_register_gate(NULL, "sdh1_clk", "sdh1_mix_clk",
				    CLK_SET_RATE_PARENT,
				    pxa_unit->apmu_base + APMU_SDH1,
				    0x12, 0x12, 0x0, 0, &sdh1_lock);
	mmp_clk_add(unit, PXA988_CLK_SDH1, clk);

	/* Register SDH2 (eMMC) mix clock and functional gate */
	sdh2_mix_config.reg_info.reg_clk_ctrl = pxa_unit->apmu_base + APMU_SDH2;
	clk = mmp_clk_register_mix(NULL, "sdh2_mix_clk", (const char **)sdh_parent_names,
				   ARRAY_SIZE(sdh_parent_names),
				   CLK_SET_RATE_PARENT,
				   &sdh2_mix_config, &sdh2_lock);
	clk = mmp_clk_register_gate(NULL, "sdh2_clk", "sdh2_mix_clk",
				    CLK_SET_RATE_PARENT,
				    pxa_unit->apmu_base + APMU_SDH2,
				    0x12, 0x12, 0x0, 0, &sdh2_lock);
	mmp_clk_add(unit, PXA988_CLK_SDH2, clk);

	/* Register general APMU gates */
	mmp_register_gate_clks(unit, apmu_gate_clks, pxa_unit->apmu_base,
			       ARRAY_SIZE(apmu_gate_clks));
}

static void __init pxa988_clk_reset_init(struct device_node *np,
					 struct pxa988_clk_unit *pxa_unit)
{
	struct mmp_clk_reset_cell *cells;
	int i, base, nr_resets_apbc, nr_resets_apbcp, nr_resets;

	nr_resets_apbc = ARRAY_SIZE(apbc_gate_clks);
	nr_resets_apbcp = ARRAY_SIZE(apbcp_gate_clks);
	nr_resets = nr_resets_apbc + nr_resets_apbcp;
	cells = kcalloc(nr_resets, sizeof(*cells), GFP_KERNEL);
	if (!cells)
		return;

	base = 0;
	for (i = 0; i < nr_resets_apbc; i++) {
		cells[base + i].clk_id = apbc_gate_clks[i].id;
		cells[base + i].reg =
			pxa_unit->apbc_base + apbc_gate_clks[i].offset;
		cells[base + i].flags = 0;
		cells[base + i].lock = apbc_gate_clks[i].lock;
		cells[base + i].bits = 0x4;
	}

	base = nr_resets_apbc;
	for (i = 0; i < nr_resets_apbcp; i++) {
		cells[base + i].clk_id = apbcp_gate_clks[i].id;
		cells[base + i].reg =
			pxa_unit->apbcp_base + apbcp_gate_clks[i].offset;
		cells[base + i].flags = 0;
		cells[base + i].lock = apbcp_gate_clks[i].lock;
		cells[base + i].bits = 0x4;
	}

	mmp_clk_reset_register(np, cells, nr_resets);
}

static void __init pxa988_clk_misc_init(struct pxa988_clk_unit *pxa_unit)
{
	u32 val;

	/* Enable SQU MP3 playback sleep mode */
	val = readl_relaxed(pxa_unit->apmu_base + APMU_SQU_CLK_GATE);
	val |= BIT(30);
	writel_relaxed(val, pxa_unit->apmu_base + APMU_SQU_CLK_GATE);

	/* Select I2S clock from VCTCXO for low power audio */
	val = readl_relaxed(pxa_unit->mpmu_base + MPMU_FCCR);
	val |= BIT(28);
	writel_relaxed(val, pxa_unit->mpmu_base + MPMU_FCCR);

	/* Always keep IPC and RIPC components enabled */
	writel_relaxed(0x3, pxa_unit->apbc_base + APBC_IPC);
	writel_relaxed(0x0, pxa_unit->apbcp_base + APBCP_RIPC);
	writel_relaxed(0x3, pxa_unit->apmu_base + APMU_MCK4_CTL);

	/* Deassert SD Host controller reset and ensure AXI bus clock enabled */
	val = readl_relaxed(pxa_unit->apmu_base + APMU_SDH0);
	val |= (SDH_ALL_RESET | SDH_ACLK_EN);
	writel_relaxed(val, pxa_unit->apmu_base + APMU_SDH0);

	/* Initialize eMMC SDH2 clock to 52MHz (PLL1_416 / 8) and clock active */
	val = readl_relaxed(pxa_unit->apmu_base + APMU_SDH2);
	writel_relaxed((val & ~0x7c0) | 0xf12, pxa_unit->apmu_base + APMU_SDH2);

	/*
	 * Configure MMC1 (microSD) pad domain to 3.3V and power active.
	 * APBC ASFAR/ASSAR must be unlocked with magic keys 0xbaba / 0xeb10.
	 */
	{
		void __iomem *aib_mmc = ioremap(0xd401e81c, 4);

		if (aib_mmc) {
			u32 sdpad;

			writel_relaxed(0xbaba, pxa_unit->apbc_base + 0x50);
			writel_relaxed(0xeb10, pxa_unit->apbc_base + 0x54);
			sdpad = (readl_relaxed(aib_mmc) & ~0xc) | 1;
			writel_relaxed(0xbaba, pxa_unit->apbc_base + 0x50);
			writel_relaxed(0xeb10, pxa_unit->apbc_base + 0x54);
			writel_relaxed(sdpad, aib_mmc);
			iounmap(aib_mmc);
		}
	}

	/* Disable unused NAND (DFC) and AES/GEU controller clocks */
	writel_relaxed(0x0, pxa_unit->apmu_base + APMU_DFC);
	writel_relaxed(0x0, pxa_unit->apmu_base + APMU_GEU);

	/* Setup CIU GPU timing registers */
	writel_relaxed(0x00055555, pxa_unit->ciu_base + CIU_GPU_XTC);
	writel_relaxed(0x00055555, pxa_unit->ciu_base + CIU_GPU2D_XTC);

	/* Enable MC4 and AXI fabric dynamic clock gating */
	val = readl_relaxed(pxa_unit->ciu_base + CIU_MC_CONF);
	val &= ~BIT(16);			/* disable cp fabric gating */
	val &= ~BIT(19);			/* enable dclk gating */
	val |= BIT(29) | BIT(30);		/* enable 1x2 fabric AXI gating */
	val |= (0xff << 8) |			/* MCK4 P0~P7 */
	       BIT(17) | BIT(18) |		/* Fabric 0 */
	       BIT(20) | BIT(21) |		/* VPU fabric */
	       BIT(26) | BIT(27);		/* Fabric 0/1 */
	writel_relaxed(val, pxa_unit->ciu_base + CIU_MC_CONF);
}

static void __init pxa988_clk_init(struct device_node *np)
{
	struct pxa988_clk_unit *pxa_unit;

	pxa_unit = kzalloc(sizeof(*pxa_unit), GFP_KERNEL);
	if (!pxa_unit)
		return;

	pxa_unit->mpmu_base = of_iomap(np, 0);
	if (!pxa_unit->mpmu_base) {
		pr_err("pxa988-clk: failed to map mpmu registers\n");
		goto free_memory;
	}

	pxa_unit->apmu_base = of_iomap(np, 1);
	if (!pxa_unit->apmu_base) {
		pr_err("pxa988-clk: failed to map apmu registers\n");
		goto unmap_mpmu;
	}

	pxa_unit->apbc_base = of_iomap(np, 2);
	if (!pxa_unit->apbc_base) {
		pr_err("pxa988-clk: failed to map apbc registers\n");
		goto unmap_apmu;
	}

	pxa_unit->apbcp_base = of_iomap(np, 3);
	if (!pxa_unit->apbcp_base) {
		pr_err("pxa988-clk: failed to map apbcp registers\n");
		goto unmap_apbc;
	}

	pxa_unit->apbs_base = of_iomap(np, 4);
	if (!pxa_unit->apbs_base) {
		pr_err("pxa988-clk: failed to map apbs registers\n");
		goto unmap_apbcp;
	}

	pxa_unit->ciu_base = of_iomap(np, 5);
	if (!pxa_unit->ciu_base) {
		pr_err("pxa988-clk: failed to map ciu registers\n");
		goto unmap_apbs;
	}

	mmp_clk_init(np, &pxa_unit->unit, PXA988_NR_CLKS);

	pxa988_clk_misc_init(pxa_unit);
	pxa988_pll_init(pxa_unit);
	pxa988_core_clk_init(pxa_unit);
	pxa988_apb_periph_clk_init(pxa_unit);
	pxa988_axi_periph_clk_init(pxa_unit);
	pxa988_clk_reset_init(np, pxa_unit);

	pr_info("pxa988-clk: Marvell PXA988 clock and reset driver initialized\n");
	return;

unmap_apbs:
	iounmap(pxa_unit->apbs_base);
unmap_apbcp:
	iounmap(pxa_unit->apbcp_base);
unmap_apbc:
	iounmap(pxa_unit->apbc_base);
unmap_apmu:
	iounmap(pxa_unit->apmu_base);
unmap_mpmu:
	iounmap(pxa_unit->mpmu_base);
free_memory:
	kfree(pxa_unit);
}

CLK_OF_DECLARE(pxa988_clk, "marvell,pxa988-clock", pxa988_clk_init);
