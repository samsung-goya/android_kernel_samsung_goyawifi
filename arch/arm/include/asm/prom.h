/*
 *  arch/arm/include/asm/prom.h
 *
 *  Copyright (C) 2009 Canonical Ltd. <jeremy.kerr@canonical.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */
#ifndef __ASMARM_PROM_H
#define __ASMARM_PROM_H

#ifdef CONFIG_OF

extern const struct machine_desc *setup_machine_fdt(unsigned int dt_phys);
extern void __init arm_dt_init_cpu_maps(void);

#ifdef CONFIG_ARM_SAMSUNG_PXA_BOOTFIXUP
void __init samsung_pxa_fixup_boot_fdt(void *blob, phys_addr_t dt_phys);
#else
static inline void samsung_pxa_fixup_boot_fdt(void *blob, phys_addr_t dt_phys)
{
}
#endif

#else /* CONFIG_OF */

static inline const struct machine_desc *setup_machine_fdt(unsigned int dt_phys)
{
	return NULL;
}

static inline void arm_dt_init_cpu_maps(void) { }

#endif /* CONFIG_OF */
#endif /* ASMARM_PROM_H */
