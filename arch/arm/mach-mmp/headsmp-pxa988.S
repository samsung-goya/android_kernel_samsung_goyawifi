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

/*
 * Physical reset/resume dispatch table and resume handler.
 * Placed in .data so physical addresses can be accessed with MMU off.
 */
	.data
	.align	3

#define SCU_PHYS_BASE		0xd1dfe000
#define SCU_CTRL		(SCU_PHYS_BASE + 0x00)
#define SCU_CPU_POWER_STATUS	(SCU_PHYS_BASE + 0x08)
#define SCU_INVALIDATE		(SCU_PHYS_BASE + 0x0c)

ENTRY(pxa988_cpu_reset_entry)
	mrc	p15, 0, r0, c0, c0, 5
	and	r0, r0, #15			@ fetch CPUID
	adr	r1, pxa988_cpu_reset_vector
	ldr	r2, [r1, r0, lsl #2]
	bx	r2
ENDPROC(pxa988_cpu_reset_entry)

ENTRY(pxa988_cpu_resume_handler)
	/* Invalidate I-cache + BTB */
	mov	r0, #0
	mcr	p15, 0, r0, c7, c5, 0

	/* Enable I-cache (bit 12) and branch prediction (bit 11) */
	mrc	p15, 0, r0, c1, c0, 0
	orr	r0, r0, #0x1800
	mcr	p15, 0, r0, c1, c0, 0
	isb

	/* Invalidate Cortex-A9 L1 D-cache after power down */
	bl	v7_invalidate_l1

	/* Fetch CPUID (0 or 1) */
	mrc	p15, 0, r0, c0, c0, 5
	and	r0, r0, #15
	eor	r6, r0, #1			@ r6 = other CPU (1 or 0)

	/*
	 * Acquire 2-CPU Peterson lock (no ldrex/strex with MMU off, as
	 * Strongly-Ordered DDR lacks a global exclusive monitor on PXA988).
	 */
	adr	r1, pxa988_peterson_flag
	mov	r2, #1
	str	r2, [r1, r0, lsl #2]		@ flag[me] = 1
	dmb
	adr	r3, pxa988_peterson_turn
	str	r6, [r3]			@ turn = other
	dsb
1:	ldr	r4, [r1, r6, lsl #2]		@ flag[other]
	cmp	r4, #0
	beq	2f
	ldr	r5, [r3]			@ turn
	cmp	r5, r6
	beq	1b

2:	/* Re-enable SCU + standby if cluster lost power (invalidate BEFORE enable) */
	ldr	r2, =SCU_CTRL
	ldr	r3, [r2]
	tst	r3, #1
	bne	3f
	mov	r4, #0xff
	ldr	r5, =SCU_INVALIDATE
	str	r4, [r5]
	dsb
	orr	r3, r3, #0x21
	str	r3, [r2]
	dsb

3:	/* Set this CPU's SCU power mode back to SCU_PM_NORMAL (0) */
	ldr	r2, =SCU_CPU_POWER_STATUS
	mov	r3, #0
	strb	r3, [r2, r0]

	/* Release Peterson lock */
	dmb
	mov	r2, #0
	str	r2, [r1, r0, lsl #2]		@ flag[me] = 0
	dsb
	sev

	b	cpu_resume
ENDPROC(pxa988_cpu_resume_handler)

	.align	2
	.globl	pxa988_cpu_reset_vector
pxa988_cpu_reset_vector:
	.long	0
	.long	0

pxa988_peterson_flag:
	.long	0
	.long	0

pxa988_peterson_turn:
	.long	0
