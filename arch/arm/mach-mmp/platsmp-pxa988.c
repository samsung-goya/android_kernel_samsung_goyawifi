// SPDX-License-Identifier: GPL-2.0
/*
 * Marvell PXA988 / PXA986 SMP operations, CPU hotplug, and CoreSight PMU glue
 *
 * Copyright (C) 2012 Marvell International Ltd.
 * Copyright (C) 2014 Linaro Ltd.
 */

#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/smp.h>

#include <asm/cacheflush.h>
#include <asm/cp15.h>
#include <asm/proc-fns.h>
#include <asm/smp_plat.h>
#include <asm/smp_scu.h>

#include "common.h"

#define APMU_PHYS_BASE			0xd4282800
#define APMU_PHYS_SIZE			0x400
#define CIU_PHYS_BASE			0xd4282c00
#define CIU_PHYS_SIZE			0x400
#define SCU_PHYS_BASE			0xd1dfe000
#define SCU_PHYS_SIZE			0x100
#define CTI_PHYS_BASE(cpu)		(0xd4118000 + 0x1000 * (cpu))
#define CTI_PHYS_SIZE			SZ_4K

/* APMU register offsets */
#define APMU_CORE_STATUS		0x0090
#define PMU_CC2_AP			0x0100
#define PMU_CORE1_IDLE_CFG		0x0128
#define APMU_CORE_WAKEUP_CTL		0x012c

/* APMU bit definitions */
#define PMU_CC2_AP_CPU1_RST		BIT(20)
#define PMU_CC2_AP_CPU1_DBG_RST		BIT(22)
#define PMU_CC2_AP_CPU1_WDOG_RST	BIT(23)
#define APMU_WAKEUP_CORE1		BIT(1)
#define APMU_CORE1_STATUS_PWRDN		BIT(7)

/* Core idle / power down flags for PMU_CORE1_IDLE_CFG */
#define PMUA_CORE_IDLE			BIT(0)
#define PMUA_CORE_POWER_DOWN		BIT(1)
#define PMUA_CORE_L1_SRAM_POWER_DOWN	BIT(2)
#define PMUA_GIC_IRQ_GLOBAL_MASK	BIT(3)
#define PMUA_GIC_FIQ_GLOBAL_MASK	BIT(4)
#define PMUA_CORE1_POWER_DOWN_MASK	(PMUA_CORE_IDLE | \
					 PMUA_CORE_POWER_DOWN | \
					 PMUA_CORE_L1_SRAM_POWER_DOWN | \
					 PMUA_GIC_IRQ_GLOBAL_MASK | \
					 PMUA_GIC_FIQ_GLOBAL_MASK)

/* CIU register offsets */
#define CIU_CPU_CORE0_CONF		0x00d0
#define CIU_WARM_RESET_VECTOR		0x00d8
#define CIU_CPU_CORE1_CONF		0x00e0
#define CIU_CONF_EXT_AGENT_EN		BIT(20)

/* CoreSight CTI registers */
#define CTI_CTRL			0x000
#define CTI_INTACK			0x010
#define CTI_EN_IN1			0x024
#define CTI_EN_OUT6			0x0b8
#define CTI_LOCK			0xfb0
#define CTI_UNLOCK_KEY			0xc5acce55
#define CTI_INTACK_OUT6			BIT(6)

static void __iomem *scu_base;
static void __iomem *apmu_base;
static void __iomem *ciu_base;
static void __iomem *cti_base[2];

static u32 core1_saved_idle_cfg;
static bool core1_off;

void pxa988_cti_enable_cpu(unsigned int cpu)
{
	u32 val;

	if (cpu >= 2)
		return;

	/* Enable CIU external agent access for debug/CTI registers */
	if (ciu_base) {
		u32 reg = (cpu == 1) ? CIU_CPU_CORE1_CONF : CIU_CPU_CORE0_CONF;

		val = readl_relaxed(ciu_base + reg);
		val |= CIU_CONF_EXT_AGENT_EN;
		writel_relaxed(val, ciu_base + reg);
	}

	if (!cti_base[cpu])
		cti_base[cpu] = ioremap(CTI_PHYS_BASE(cpu), CTI_PHYS_SIZE);

	if (!cti_base[cpu])
		return;

	/* Unlock CTI */
	writel_relaxed(CTI_UNLOCK_KEY, cti_base[cpu] + CTI_LOCK);

	/* Route PMU overflow (CTIENIN1) to channel cpu */
	val = readl_relaxed(cti_base[cpu] + CTI_EN_IN1);
	val &= ~0x0f;
	val |= BIT(cpu);
	writel_relaxed(val, cti_base[cpu] + CTI_EN_IN1);

	/* Route channel cpu to trigger output 6 (CTIENOUT6, GIC SPI) */
	val = readl_relaxed(cti_base[cpu] + CTI_EN_OUT6);
	val &= ~0x0f;
	val |= BIT(cpu);
	writel_relaxed(val, cti_base[cpu] + CTI_EN_OUT6);

	/* Clear any pending latch */
	writel_relaxed(CTI_INTACK_OUT6, cti_base[cpu] + CTI_INTACK);

	/* Enable CTI */
	writel_relaxed(0x1, cti_base[cpu] + CTI_CTRL);
}

void pxa988_pmu_ack(void)
{
	unsigned int cpu = smp_processor_id();

	if (cpu < 2 && cti_base[cpu])
		writel_relaxed(CTI_INTACK_OUT6, cti_base[cpu] + CTI_INTACK);
}
EXPORT_SYMBOL_GPL(pxa988_pmu_ack);

static void __init pxa988_smp_prepare_cpus(unsigned int max_cpus)
{
	struct device_node *np;

	np = of_find_compatible_node(NULL, NULL, "arm,cortex-a9-scu");
	if (np) {
		scu_base = of_iomap(np, 0);
		of_node_put(np);
	}
	if (!scu_base)
		scu_base = ioremap(SCU_PHYS_BASE, SCU_PHYS_SIZE);

	if (WARN_ON(!scu_base))
		return;

	apmu_base = ioremap(APMU_PHYS_BASE, APMU_PHYS_SIZE);
	ciu_base = ioremap(CIU_PHYS_BASE, CIU_PHYS_SIZE);

	WARN_ON(!apmu_base || !ciu_base);

	scu_enable(scu_base);

	/* Initialize CoreSight CTI for CPU0 */
	pxa988_cti_enable_cpu(0);
}

static int pxa988_smp_boot_secondary(unsigned int cpu, struct task_struct *idle)
{
	u32 reset, entry;

	if (cpu != 1)
		return -EINVAL;

	if (!apmu_base || !ciu_base)
		return -EFAULT;

	if (core1_off) {
		if (scu_base)
			scu_cpu_power_enable(scu_base, cpu);
		writel_relaxed(core1_saved_idle_cfg, apmu_base + PMU_CORE1_IDLE_CFG);
		core1_off = false;
		/* Ensure core1_off clear is visible before CPU1 reset release */
		smp_wmb();
	}

	entry = (u32)__pa_symbol(pxa988_secondary_startup);

	/* No TrustZone hypervisor on goyawifi. Boot ROM reads DDR vector. */
	writel_relaxed(entry, ciu_base + CIU_WARM_RESET_VECTOR);
	readl_relaxed(ciu_base + CIU_WARM_RESET_VECTOR);
	dsb(sy);

	reset = readl_relaxed(apmu_base + PMU_CC2_AP);

	/* If bootloader left CPU1 in an old holding pen, reset it first */
	if (!(reset & PMU_CC2_AP_CPU1_RST)) {
		writel_relaxed(reset | PMU_CC2_AP_CPU1_RST, apmu_base + PMU_CC2_AP);
		readl_relaxed(apmu_base + PMU_CC2_AP);
		udelay(10);
	}

	/* Release CPU1 hardware core reset, debug logic reset, and watchdog */
	reset &= ~(PMU_CC2_AP_CPU1_RST | PMU_CC2_AP_CPU1_DBG_RST | PMU_CC2_AP_CPU1_WDOG_RST);
	writel_relaxed(reset, apmu_base + PMU_CC2_AP);
	readl_relaxed(apmu_base + PMU_CC2_AP);

	/* Send wakeup pulse to Core 1 in APMU */
	writel_relaxed(APMU_WAKEUP_CORE1, apmu_base + APMU_CORE_WAKEUP_CTL);
	dsb(sy);

	return 0;
}

static void pxa988_smp_secondary_init(unsigned int cpu)
{
	/* Re-enable CoreSight CTI on secondary core */
	pxa988_cti_enable_cpu(cpu);
}

#ifdef CONFIG_HOTPLUG_CPU
static int pxa988_cpu_disable(unsigned int cpu)
{
	return cpu == 1 ? 0 : -EPERM;
}

static void pxa988_cpu_die(unsigned int cpu)
{
	if (!apmu_base || !scu_base)
		return;

	core1_saved_idle_cfg = readl_relaxed(apmu_base + PMU_CORE1_IDLE_CFG);
	core1_off = true;
	/* Ensure core1_off flag is visible before triggering power down */
	smp_wmb();

	writel_relaxed(core1_saved_idle_cfg | PMUA_CORE1_POWER_DOWN_MASK,
		       apmu_base + PMU_CORE1_IDLE_CFG);

	v7_exit_coherency_flush(louis);
	scu_power_mode(scu_base, SCU_PM_POWEROFF);

	while (1)
		cpu_do_idle();
}

static int pxa988_cpu_kill(unsigned int cpu)
{
	u32 status;
	int ret;

	if (cpu != 1 || !apmu_base)
		return 0;

	ret = readl_poll_timeout_atomic(apmu_base + APMU_CORE_STATUS, status,
					status & APMU_CORE1_STATUS_PWRDN,
					10, 100000);

	return ret == 0;
}
#endif

static const struct smp_operations pxa988_smp_ops __initconst = {
	.smp_prepare_cpus	= pxa988_smp_prepare_cpus,
	.smp_boot_secondary	= pxa988_smp_boot_secondary,
	.smp_secondary_init	= pxa988_smp_secondary_init,
#ifdef CONFIG_HOTPLUG_CPU
	.cpu_disable		= pxa988_cpu_disable,
	.cpu_die		= pxa988_cpu_die,
	.cpu_kill		= pxa988_cpu_kill,
#endif
};

CPU_METHOD_OF_DECLARE(pxa988_smp, "marvell,pxa988-smp", &pxa988_smp_ops);
CPU_METHOD_OF_DECLARE(pxa986_smp, "marvell,pxa986-smp", &pxa988_smp_ops);
