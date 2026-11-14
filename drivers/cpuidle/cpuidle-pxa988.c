// SPDX-License-Identifier: GPL-2.0
/*
 * Marvell PXA988 / PXA986 CPUidle driver
 *
 * Copyright (C) 2012-2013 Marvell International Ltd.
 */

#include <linux/cpuidle.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>

#include <asm/cpuidle.h>

static int (*pxa988_lpm_enter)(int state);

static int pxa988_cpuidle_enter(struct cpuidle_device *dev,
				struct cpuidle_driver *drv, int index)
{
	if (pxa988_lpm_enter)
		pxa988_lpm_enter(index);
	else
		cpu_do_idle();

	return index;
}

static struct cpuidle_driver pxa988_idle_driver = {
	.name			= "pxa988_idle",
	.owner			= THIS_MODULE,
	.states[0]		= ARM_CPUIDLE_WFI_STATE,
	.states[1]		= {
		.enter			= pxa988_cpuidle_enter,
		.exit_latency		= 100,
		.target_residency	= 250,
		.flags			= CPUIDLE_FLAG_TIMER_STOP,
		.name			= "C2",
		.desc			= "Core power down, L1 off",
	},
	/*
	 * D1P/D1 shut down AXI/APB/DDR without peripheral pm_qos busy guards,
	 * which can stall active i2c-pxa / sdhci-pxav3 transfers mid-flight.
	 * Keep cpuidle at C1/C2 and reserve full SoC power-down for D2_UDR.
	 */
	.state_count = 2,
};

static int pxa988_cpuidle_probe(struct platform_device *pdev)
{
	pxa988_lpm_enter = pdev->dev.platform_data;
	if (!pxa988_lpm_enter)
		return -EINVAL;

	return cpuidle_register(&pxa988_idle_driver, NULL);
}

static struct platform_driver pxa988_cpuidle_plat_driver = {
	.probe	= pxa988_cpuidle_probe,
	.driver	= {
		.name	= "cpuidle-pxa988",
	},
};

builtin_platform_driver(pxa988_cpuidle_plat_driver);
