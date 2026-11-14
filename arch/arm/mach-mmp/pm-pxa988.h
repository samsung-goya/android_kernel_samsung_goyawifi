/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __MACH_MMP_PM_PXA988_H
#define __MACH_MMP_PM_PXA988_H

enum pxa988_lpm_state {
	PXA988_LPM_C1 = 0,
	PXA988_LPM_C2,
	PXA988_LPM_D1P,
	PXA988_LPM_D1,
	PXA988_LPM_D2_UDR,
};

#ifdef CONFIG_PM
int pxa988_enter_lowpower(enum pxa988_lpm_state state);
#else
static inline int pxa988_enter_lowpower(enum pxa988_lpm_state state)
{
	return 0;
}
#endif

#endif /* __MACH_MMP_PM_PXA988_H */
