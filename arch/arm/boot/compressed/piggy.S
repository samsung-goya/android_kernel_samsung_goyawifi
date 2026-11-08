/* SPDX-License-Identifier: GPL-2.0 */
	.section .piggydata, "a"
	.p2align 2
	.globl	input_data
input_data:
	.incbin	"arch/arm/boot/compressed/piggy_data"
	.globl	input_data_end
input_data_end:
