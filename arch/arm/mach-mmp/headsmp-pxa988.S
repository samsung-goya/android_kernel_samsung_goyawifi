/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Marvell PXA988 / PXA986 secondary CPU startup
 *
 * Copyright (C) 2012 Marvell International Ltd.
 * Neil Zhang <zhangwm@marvell.com>
 */

#include <linux/linkage.h>

	.arm
	.align 3

/*
 * Entry point for secondary CPUs released from reset by CIU_WARM_RESET_VECTOR.
 * Architecturally, Cortex-A9 L1 data cache lines are undefined after cold
 * reset and must be invalidated prior to turning on MMU and coherency.
 */
ENTRY(pxa988_secondary_startup)
	bl	v7_invalidate_l1
	b	secondary_startup
ENDPROC(pxa988_secondary_startup)
