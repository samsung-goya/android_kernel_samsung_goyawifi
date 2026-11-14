// SPDX-License-Identifier: GPL-2.0
/*
 * Marvell PXA988 / PXA986 low-power idle and system suspend support
 *
 * Copyright (C) 2012-2013 Marvell International Ltd.
 */

#include <linux/clk.h>
#include <linux/cpu_pm.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <linux/suspend.h>

#include <asm/cacheflush.h>
#include <asm/cp15.h>
#include <asm/hardware/cache-l2x0.h>
#include <asm/proc-fns.h>
#include <asm/smp_scu.h>
#include <asm/suspend.h>

#include "common.h"
#include "pm-pxa988.h"

/* Physical register bases */
#define APBC_PHYS_BASE			0xd4015000
#define APBC_PHYS_SIZE			0x100
#define GPIOE_PHYS_BASE			0xd4019800
#define GPIOE_PHYS_SIZE			0x100
#define MFPR_PHYS_BASE			0xd401e000
#define MFPR_PHYS_SIZE			0x400
#define MPMU_PHYS_BASE			0xd4050000
#define MPMU_PHYS_SIZE			0x2000
#define ICU_PHYS_BASE			0xd4282000
#define ICU_PHYS_SIZE			0x400
#define APMU_PHYS_BASE			0xd4282800
#define APMU_PHYS_SIZE			0x400
#define CIU_PHYS_BASE			0xd4282c00
#define CIU_PHYS_SIZE			0x400
#define SCU_PHYS_BASE			0xd1dfe000
#define SCU_PHYS_SIZE			0x100
#define GIC_DIST_PHYS_BASE		0xd1dff000
#define GIC_DIST_PHYS_SIZE		0x1000

/* APBC */
#define APBC_KPC			0x0030

/* MPMU */
#define MPMU_SD_ROT_WAKE_CLR		0x007c
#define MPMU_SD_ROT_WAKE_CLR_ALL	BIT(3)
#define MPMU_APCR			0x1000
#define MPMU_AWUCRM			0x104c

/* MPMU_APCR bits */
#define PMUM_AXISD			BIT(31)
#define PMUM_DSPSD			BIT(30)
#define PMUM_SLPEN			BIT(29)
#define PMUM_DTCMSD			BIT(28)
#define PMUM_DDRCORSD			BIT(27)
#define PMUM_APBSD			BIT(26)
#define PMUM_BBSD			BIT(25)
#define PMUM_INTCLR			BIT(24)
#define PMUM_SLPWP0			BIT(23)
#define PMUM_SLPWP1			BIT(22)
#define PMUM_SLPWP2			BIT(21)
#define PMUM_SLPWP3			BIT(20)
#define PMUM_VCTCXOSD			BIT(19)
#define PMUM_SLPWP4			BIT(18)
#define PMUM_SLPWP5			BIT(17)
#define PMUM_SLPWP6			BIT(16)
#define PMUM_SLPWP7			BIT(15)
#define PMUM_MSASLPEN			BIT(14)
#define PMUM_STBYEN			BIT(13)

#define PXA986_APCR_BASELINE		(PMUM_DTCMSD | PMUM_BBSD | PMUM_MSASLPEN)
#define PXA986_APCR_LPM_BITS		(PMUM_AXISD | PMUM_DDRCORSD | PMUM_SLPEN | \
					 PMUM_APBSD | PMUM_STBYEN | PMUM_VCTCXOSD)

/* MPMU_AWUCRM bits */
#define PMUM_AP_ASYNC_INT		BIT(25)
#define PMUM_AP_FULL_IDLE		BIT(24)
#define PMUM_SQU_SDH1			BIT(23)
#define PMUM_SDH_23			BIT(22)
#define PMUM_KEYPRESS			BIT(21)
#define PMUM_RTC_ALARM			BIT(17)
#define PMUM_AP2_TIMER_3		BIT(13)
#define PMUM_AP2_TIMER_2		BIT(12)
#define PMUM_AP2_TIMER_1		BIT(11)
#define PMUM_AP1_TIMER_3		BIT(10)
#define PMUM_AP1_TIMER_2		BIT(9)
#define PMUM_AP1_TIMER_1		BIT(8)
#define PMUM_WAKEUP7			BIT(7)
#define PMUM_WAKEUP6			BIT(6)
#define PMUM_WAKEUP5			BIT(5)
#define PMUM_WAKEUP4			BIT(4)
#define PMUM_WAKEUP3			BIT(3)
#define PMUM_WAKEUP2			BIT(2)
#define PMUM_WAKEUP1			BIT(1)
#define PMUM_WAKEUP0			BIT(0)

/*
 * PXA986 wakeup port mapping in MPMU_AWUCRM:
 * - WAKEUP7 (PMIC 88PM822 ONKEY / RTC / charger)
 * - WAKEUP3 (GPIO_AP / MFPR pad edge)
 * - WAKEUP2 (SDH)
 * - RTC_ALARM (internal SoC RTC)
 * - AP1_TIMER_1..3 (broadcast clockevent timer)
 */
#define PXA986_AWUCRM_IDLE_MASK		(PMUM_AP_ASYNC_INT | PMUM_AP_FULL_IDLE | \
					 PMUM_AP1_TIMER_1 | PMUM_AP1_TIMER_2 | \
					 PMUM_AP1_TIMER_3 | PMUM_WAKEUP7 | \
					 PMUM_WAKEUP3 | PMUM_WAKEUP2 | \
					 PMUM_RTC_ALARM)

#define PXA986_AWUCRM_SUSPEND_MASK	(PMUM_AP_ASYNC_INT | PMUM_AP_FULL_IDLE | \
					 PMUM_WAKEUP7 | PMUM_WAKEUP3 | \
					 PMUM_WAKEUP2 | PMUM_RTC_ALARM)

/* APMU */
#define APMU_DEBUG			0x0088
#define APMU_DEBUG_CP_HALT		BIT(0)
#define APMU_DEBUG_CP_CLK_OFF_ACK	BIT(3)
#define APMU_MP_IDLE_CFG0		0x0120
#define APMU_CORE0_IDLE_CFG		0x0124
#define APMU_CORE1_IDLE_CFG		0x0128

#define PMUA_CORE_IDLE			BIT(0)
#define PMUA_CORE_POWER_DOWN		BIT(1)
#define PMUA_CORE_L1_SRAM_POWER_DOWN	BIT(2)
#define PMUA_GIC_IRQ_GLOBAL_MASK	BIT(3)
#define PMUA_GIC_FIQ_GLOBAL_MASK	BIT(4)
#define PMUA_CORE_IDLE_MASK		(PMUA_CORE_IDLE | PMUA_CORE_POWER_DOWN | \
					 PMUA_CORE_L1_SRAM_POWER_DOWN | \
					 PMUA_GIC_IRQ_GLOBAL_MASK | \
					 PMUA_GIC_FIQ_GLOBAL_MASK)

#define PMUA_MP_IDLE			BIT(0)
#define PMUA_MP_POWER_DOWN		BIT(1)
#define PMUA_MP_L2_SRAM_POWER_DOWN	BIT(2)
#define PMUA_MP_SCU_SRAM_POWER_DOWN	BIT(3)
#define PMUA_MP_MASK_CLK_OFF		BIT(11)
#define PMUA_DIS_MP_SLP			BIT(18)
#define PMUA_MP_IDLE_MASK		(PMUA_MP_IDLE | PMUA_MP_POWER_DOWN | \
					 PMUA_MP_L2_SRAM_POWER_DOWN | \
					 PMUA_MP_SCU_SRAM_POWER_DOWN | \
					 PMUA_MP_MASK_CLK_OFF)

/* CIU */
#define CIU_WARM_RESET_VECTOR		0x00d8

/* ICU */
#define ICU_INT_CONF(n)			((n) << 2)
#define ICU_INT_CONF_AP_INT		BIT(6)
#define ICU_INT_CONF_IRQ		BIT(4)
#define ICU_SPI_COUNT			96
#define ICU_SPI_GPIO_EDGE		50

/* GIC Distributor */
#define GIC_DIST_ENABLE_SET		0x100
#define GIC_DIST_PENDING_SET		0x200

/* MFPR edge detect bits */
#define MFPR_EDGE_RISE_EN		BIT(4)
#define MFPR_EDGE_FALL_EN		BIT(5)
#define MFPR_EDGE_CLEAR			BIT(6)

static void __iomem *apbc_base;
static void __iomem *mfpr_base;
static void __iomem *mpmu_base;
static void __iomem *icu_base;
static void __iomem *apmu_base;
static void __iomem *ciu_base;
static void __iomem *scu_base;
static void __iomem *gic_dist_base;

static DEFINE_RAW_SPINLOCK(pxa988_lpm_lock);
static unsigned long cpus_in_lpm;
static enum pxa988_lpm_state cpu_lpm_vote[2];
static bool mp_lpm_armed;
static enum pxa988_lpm_state mp_lpm_state;

extern void pxa_gpio_for_each_wake(void (*fn)(unsigned int gpio, bool rise,
					       bool fall, void *data),
				   void *data);

static inline u32 pxa988_core_idle_cfg_off(unsigned int cpu)
{
	return cpu == 0 ? APMU_CORE0_IDLE_CFG : APMU_CORE1_IDLE_CFG;
}

static int pxa988_gpio_to_mfpr_offset(unsigned int gpio)
{
	if (gpio <= 54)
		return 0x00dc + 4 * gpio;
	if (gpio >= 67 && gpio <= 98)
		return 0x01b8 + 4 * (gpio - 67);
	if (gpio >= 100 && gpio <= 112)
		return 0x0238 + 4 * (gpio - 100);
	return -1;
}

static void pxa988_mfpr_arm_wake(unsigned int gpio, bool rise, bool fall,
				 void *data)
{
	int offset = pxa988_gpio_to_mfpr_offset(gpio);
	bool *has_wake = data;
	u32 val;

	if (offset < 0 || !mfpr_base)
		return;

	val = readl_relaxed(mfpr_base + offset);
	val &= ~(MFPR_EDGE_CLEAR | MFPR_EDGE_RISE_EN | MFPR_EDGE_FALL_EN);
	if (rise)
		val |= MFPR_EDGE_RISE_EN;
	if (fall)
		val |= MFPR_EDGE_FALL_EN;
	writel_relaxed(val, mfpr_base + offset);

	if (has_wake)
		*has_wake = true;
}

static void pxa988_mfpr_disarm_wake(unsigned int gpio, bool rise, bool fall,
				    void *data)
{
	int offset = pxa988_gpio_to_mfpr_offset(gpio);
	u32 val;

	if (offset < 0 || !mfpr_base)
		return;

	val = readl_relaxed(mfpr_base + offset);
	val &= ~(MFPR_EDGE_RISE_EN | MFPR_EDGE_FALL_EN);
	val |= MFPR_EDGE_CLEAR;
	writel_relaxed(val, mfpr_base + offset);
}

static void pxa988_icu_mirror_gic(bool arm_gpio_edge)
{
	unsigned int i;

	for (i = 0; i < ICU_SPI_COUNT; i++) {
		u32 gic_irq = i + 32;
		u32 enable = readl_relaxed(gic_dist_base + GIC_DIST_ENABLE_SET +
					   (gic_irq / 32) * 4);
		u32 val = 0;

		if (enable & BIT(gic_irq % 32))
			val = ICU_INT_CONF_AP_INT | ICU_INT_CONF_IRQ | 0x1;
		else if (arm_gpio_edge && i == ICU_SPI_GPIO_EDGE)
			val = ICU_INT_CONF_AP_INT | ICU_INT_CONF_IRQ | 0x1;

		writel_relaxed(val, icu_base + ICU_INT_CONF(i));
	}
}

static void core_enter_coherency(void)
{
	unsigned int v;

	asm volatile(
	"	mrc	p15, 0, %0, c1, c0, 1\n"
	"	orr	%0, %0, #(1 << 6)\n"
	"	mcr	p15, 0, %0, c1, c0, 1\n"
	"	isb\n"
	: "=&r" (v) : : "cc");
}

static void core_exit_coherency(void)
{
	unsigned int v;

	asm volatile(
	"	mrc	p15, 0, %0, c1, c0, 1\n"
	"	bic	%0, %0, #(1 << 6)\n"
	"	mcr	p15, 0, %0, c1, c0, 1\n"
	"	isb\n"
	: "=&r" (v) : : "cc");
}

static void disable_l1_dcache(void)
{
	unsigned int v;

	asm volatile(
	"	mrc	p15, 0, %0, c1, c0, 0\n"
	"	bic	%0, %0, #(1 << 2)\n"
	"	mcr	p15, 0, %0, c1, c0, 0\n"
	"	isb\n"
	: "=&r" (v) : : "cc");
}

static void enable_l1_dcache(void)
{
	unsigned int v;

	asm volatile(
	"	mrc	p15, 0, %0, c1, c0, 0\n"
	"	orr	%0, %0, #(1 << 2)\n"
	"	mcr	p15, 0, %0, c1, c0, 0\n"
	"	isb\n"
	: "=&r" (v) : : "cc");
}

static u32 d2_abort_gic_idx;
static u32 d2_abort_gic_val;

static bool pxa988_gic_pending(enum pxa988_lpm_state state)
{
	unsigned int i;

	for (i = 0; i < 4; i++) {
		u32 pending = readl_relaxed(gic_dist_base +
					    GIC_DIST_PENDING_SET + i * 4);
		u32 enabled = readl_relaxed(gic_dist_base +
					    GIC_DIST_ENABLE_SET + i * 4);

		if (pending & enabled) {
			if (state == PXA988_LPM_D2_UDR) {
				d2_abort_gic_idx = i;
				d2_abort_gic_val = pending & enabled;
			}
			return true;
		}
	}
	return false;
}

static int pxa988_lowpower_finish(unsigned long param)
{
	enum pxa988_lpm_state state = (enum pxa988_lpm_state)param;

	flush_cache_all();
	disable_l1_dcache();
	core_exit_coherency();

	if (scu_base)
		scu_power_mode(scu_base, SCU_PM_POWEROFF);

	if (state == PXA988_LPM_D2_UDR)
		outer_disable();

	if (pxa988_gic_pending(state))
		goto abort;

	cpu_do_idle();

abort:
	if (scu_base)
		scu_power_mode(scu_base, SCU_PM_NORMAL);
	core_enter_coherency();
	enable_l1_dcache();

	return 0;
}

static void pxa988_set_pmu(unsigned int cpu, enum pxa988_lpm_state state,
			   bool last_man)
{
	u32 core_cfg, mp_cfg, apcr, awucrm;
	bool has_wake_gpio = false;

	core_cfg = readl_relaxed(apmu_base + pxa988_core_idle_cfg_off(cpu));
	core_cfg &= ~PMUA_CORE_IDLE_MASK;
	core_cfg |= PMUA_CORE_IDLE | PMUA_CORE_POWER_DOWN |
		    PMUA_CORE_L1_SRAM_POWER_DOWN |
		    PMUA_GIC_IRQ_GLOBAL_MASK | PMUA_GIC_FIQ_GLOBAL_MASK;
	writel_relaxed(core_cfg, apmu_base + pxa988_core_idle_cfg_off(cpu));

	if (!last_man || state == PXA988_LPM_C2)
		return;

	mp_cfg = readl_relaxed(apmu_base + APMU_MP_IDLE_CFG0);
	mp_cfg &= ~(PMUA_MP_IDLE_MASK | PMUA_DIS_MP_SLP);
	mp_cfg |= PMUA_MP_IDLE | PMUA_MP_POWER_DOWN |
		  PMUA_MP_SCU_SRAM_POWER_DOWN | PMUA_MP_MASK_CLK_OFF;
	if (state == PXA988_LPM_D2_UDR)
		mp_cfg |= PMUA_MP_L2_SRAM_POWER_DOWN;
	writel_relaxed(mp_cfg, apmu_base + APMU_MP_IDLE_CFG0);

	apcr = readl_relaxed(mpmu_base + MPMU_APCR);
	apcr &= ~(PXA986_APCR_LPM_BITS | PMUM_DSPSD);
	apcr |= PXA986_APCR_BASELINE;

	switch (state) {
	case PXA988_LPM_D2_UDR:
		apcr |= PMUM_VCTCXOSD | PMUM_STBYEN;
		/* fall through */
	case PXA988_LPM_D1:
		apcr |= PMUM_APBSD | PMUM_SLPEN | PMUM_DDRCORSD;
		/* fall through */
	case PXA988_LPM_D1P:
		apcr |= PMUM_AXISD;
		break;
	default:
		break;
	}
	writel_relaxed(apcr, mpmu_base + MPMU_APCR);

	awucrm = (state == PXA988_LPM_D2_UDR) ?
		 PXA986_AWUCRM_SUSPEND_MASK : PXA986_AWUCRM_IDLE_MASK;
	writel_relaxed(awucrm, mpmu_base + MPMU_AWUCRM);

	pxa_gpio_for_each_wake(pxa988_mfpr_arm_wake, &has_wake_gpio);
	pxa988_icu_mirror_gic(has_wake_gpio);
}

static void pxa988_clear_pmu(unsigned int cpu, bool first_man,
			     enum pxa988_lpm_state armed_state)
{
	u32 core_cfg, mp_cfg, apcr;

	core_cfg = readl_relaxed(apmu_base + pxa988_core_idle_cfg_off(cpu));
	core_cfg &= ~PMUA_CORE_IDLE_MASK;
	writel_relaxed(core_cfg, apmu_base + pxa988_core_idle_cfg_off(cpu));

	if (!first_man)
		return;

	mp_cfg = readl_relaxed(apmu_base + APMU_MP_IDLE_CFG0);
	mp_cfg &= ~PMUA_MP_IDLE_MASK;
	mp_cfg |= PMUA_DIS_MP_SLP;
	writel_relaxed(mp_cfg, apmu_base + APMU_MP_IDLE_CFG0);

	apcr = readl_relaxed(mpmu_base + MPMU_APCR);
	apcr &= ~(PXA986_APCR_LPM_BITS | PMUM_DSPSD);
	apcr |= PXA986_APCR_BASELINE;
	writel_relaxed(apcr, mpmu_base + MPMU_APCR);

	pxa_gpio_for_each_wake(pxa988_mfpr_disarm_wake, NULL);

	if (armed_state == PXA988_LPM_D2_UDR && ciu_base) {
		writel_relaxed((u32)__pa_symbol(pxa988_cpu_reset_entry),
			       ciu_base + CIU_WARM_RESET_VECTOR);
		readl_relaxed(ciu_base + CIU_WARM_RESET_VECTOR);
	}
}

int pxa988_enter_lowpower(enum pxa988_lpm_state state)
{
	unsigned int cpu = smp_processor_id();
	enum pxa988_lpm_state cluster_state = PXA988_LPM_C2;
	bool last_man = false;
	bool first_man = false;
	enum pxa988_lpm_state armed_state = PXA988_LPM_C2;
	int ret;

	if (state == PXA988_LPM_C1) {
		cpu_do_idle();
		return 0;
	}

	if (!apmu_base || !mpmu_base || !icu_base || !gic_dist_base) {
		cpu_do_idle();
		return 0;
	}

	raw_spin_lock(&pxa988_lpm_lock);
	cpus_in_lpm |= BIT(cpu);
	cpu_lpm_vote[cpu] = state;

	if ((cpus_in_lpm & cpumask_bits(cpu_online_mask)[0]) ==
	    cpumask_bits(cpu_online_mask)[0]) {
		unsigned int c;

		cluster_state = state;
		for_each_online_cpu(c) {
			if (cpu_lpm_vote[c] < cluster_state)
				cluster_state = cpu_lpm_vote[c];
		}
		if (cluster_state >= PXA988_LPM_D1P) {
			last_man = true;
			mp_lpm_armed = true;
			mp_lpm_state = cluster_state;
		}
	}

	pxa988_set_pmu(cpu, last_man ? cluster_state : PXA988_LPM_C2, last_man);
	if (!last_man)
		pxa988_icu_mirror_gic(false);
	raw_spin_unlock(&pxa988_lpm_lock);

	cpu_pm_enter();
	if (last_man)
		cpu_cluster_pm_enter();

	ret = cpu_suspend((unsigned long)(last_man ? cluster_state : PXA988_LPM_C2),
			  pxa988_lowpower_finish);

	if (state == PXA988_LPM_D2_UDR)
		outer_resume();

	raw_spin_lock(&pxa988_lpm_lock);
	if (mp_lpm_armed) {
		first_man = true;
		armed_state = mp_lpm_state;
		mp_lpm_armed = false;
	}
	pxa988_clear_pmu(cpu, first_man, armed_state);
	cpus_in_lpm &= ~BIT(cpu);
	cpu_lpm_vote[cpu] = PXA988_LPM_C1;
	raw_spin_unlock(&pxa988_lpm_lock);

	if (first_man)
		cpu_cluster_pm_exit();
	cpu_pm_exit();

	return ret;
}
EXPORT_SYMBOL_GPL(pxa988_enter_lowpower);

static int pxa988_pm_enter(suspend_state_t state)
{
	int ret;

	if (state != PM_SUSPEND_MEM)
		return -EINVAL;

	d2_abort_gic_val = 0;
	ret = pxa988_enter_lowpower(PXA988_LPM_D2_UDR);
	if (d2_abort_gic_val)
		pr_info("pxa988: D2_UDR aborted by pending GIC[%u]=0x%08x\n",
			d2_abort_gic_idx, d2_abort_gic_val);
	pr_info("pxa988: D2_UDR exit (cpu_suspend=%d)\n", ret);
	return 0;
}

static const struct platform_suspend_ops pxa988_pm_ops = {
	.valid	= suspend_valid_only_mem,
	.enter	= pxa988_pm_enter,
};

static int pxa988_cpuidle_enter_cb(int state)
{
	return pxa988_enter_lowpower((enum pxa988_lpm_state)state);
}

static struct platform_device pxa988_cpuidle_pdev = {
	.name	= "cpuidle-pxa988",
	.id	= -1,
	.dev	= {
		.platform_data = pxa988_cpuidle_enter_cb,
	},
};

static int __init pxa988_pm_init(void)
{
	u32 val;

	if (!of_machine_is_compatible("marvell,pxa988") &&
	    !of_machine_is_compatible("marvell,pxa986"))
		return 0;

	apbc_base = ioremap(APBC_PHYS_BASE, APBC_PHYS_SIZE);
	mfpr_base = ioremap(MFPR_PHYS_BASE, MFPR_PHYS_SIZE);
	mpmu_base = ioremap(MPMU_PHYS_BASE, MPMU_PHYS_SIZE);
	icu_base = ioremap(ICU_PHYS_BASE, ICU_PHYS_SIZE);
	apmu_base = ioremap(APMU_PHYS_BASE, APMU_PHYS_SIZE);
	ciu_base = ioremap(CIU_PHYS_BASE, CIU_PHYS_SIZE);
	scu_base = ioremap(SCU_PHYS_BASE, SCU_PHYS_SIZE);
	gic_dist_base = ioremap(GIC_DIST_PHYS_BASE, GIC_DIST_PHYS_SIZE);

	if (WARN_ON(!apmu_base || !mpmu_base || !icu_base || !gic_dist_base))
		return -ENOMEM;

	/* Disable unused keypad controller clock that can block D1/D2 */
	if (apbc_base)
		writel_relaxed(0, apbc_base + APBC_KPC);

	/* Clear SD/rotary wake status latch */
	val = readl_relaxed(mpmu_base + MPMU_SD_ROT_WAKE_CLR);
	writel_relaxed(val | MPMU_SD_ROT_WAKE_CLR_ALL,
		       mpmu_base + MPMU_SD_ROT_WAKE_CLR);

	/* Allow AP idle even when CP is halted / clock-off */
	val = readl_relaxed(apmu_base + APMU_DEBUG);
	writel_relaxed(val | APMU_DEBUG_CP_HALT | APMU_DEBUG_CP_CLK_OFF_ACK,
		       apmu_base + APMU_DEBUG);

	/* Set clean PXA986 MPMU_APCR baseline (no DSPSD, no STBYEN at idle) */
	val = readl_relaxed(mpmu_base + MPMU_APCR);
	val &= ~(PXA986_APCR_LPM_BITS | PMUM_DSPSD);
	val |= PXA986_APCR_BASELINE;
	writel_relaxed(val, mpmu_base + MPMU_APCR);

	/* Default to MP sleep disabled until last-man D1P/D1/D2 entry */
	val = readl_relaxed(apmu_base + APMU_MP_IDLE_CFG0);
	val &= ~PMUA_MP_IDLE_MASK;
	val |= PMUA_DIS_MP_SLP;
	writel_relaxed(val, apmu_base + APMU_MP_IDLE_CFG0);

	suspend_set_ops(&pxa988_pm_ops);
	platform_device_register(&pxa988_cpuidle_pdev);

	pr_info("pxa988: low-power idle and suspend initialized\n");
	return 0;
}
late_initcall(pxa988_pm_init);
