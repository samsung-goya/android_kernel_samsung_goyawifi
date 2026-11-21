// SPDX-License-Identifier: GPL-2.0
/*
 * Bootloader FDT/initramfs handoff fixup for Samsung PXA986/PXA988 boards.
 *
 * The Samsung bootloader on SM-T110 (goyawifi) loads zImage at 0x10008000,
 * ignores the boot.img header command line, and supplies a fixed low initrd
 * address. The image packer (goyawifi-mkbootimg.py) carries the external
 * initramfs in trailing FDT space after a 4 KiB page gap so the ARM zImage
 * decompressor relocates the entire bundle together.
 *
 * Before early_init_dt_verify() computes the FDT CRC32 over the 2 MiB early
 * mapping, derive the relocated physical initrd range from dt_phys and
 * shrink fdt_totalsize back to the actual Device Tree size so the generic
 * FDT and initrd memblock reservations do not overlap.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/libfdt.h>
#include <linux/sizes.h>
#include <linux/types.h>

#include <asm/prom.h>

void __init samsung_pxa_fixup_boot_fdt(void *blob, phys_addr_t dt_phys)
{
	const fdt32_t *size_prop;
	u32 total, size, dt_size, start, end;
	u64 end64;
	int chosen, len;

	if (!blob || fdt_check_header(blob) ||
	    fdt_node_check_compatible(blob, 0, "samsung,goyawifi"))
		return;

	chosen = fdt_path_offset(blob, "/chosen");
	if (chosen < 0)
		return;

	size_prop = fdt_getprop(blob, chosen, "samsung,goyawifi-initrd-size",
				&len);
	if (!size_prop || len != sizeof(*size_prop)) {
		const fdt32_t *s_prop = fdt_getprop(blob, chosen,
						    "linux,initrd-start", &len);
		const fdt32_t *e_prop = fdt_getprop(blob, chosen,
						    "linux,initrd-end", &len);
		const fdt32_t *o_prop = fdt_getprop(blob, chosen,
						    "samsung,goyawifi-orig-initrd",
						    &len);
		const char *bcmd = fdt_getprop(blob, chosen,
					       "samsung,goyawifi-boot-cmdline",
					       &len);
		if (bcmd && len > 0)
			pr_info("goyawifi: bootloader cmdline: %s\n", bcmd);
		if (s_prop && e_prop)
			pr_info("goyawifi: bootloader initramfs %08x..%08x (orig %08x), FDT at %pa\n",
				fdt32_to_cpu(*s_prop), fdt32_to_cpu(*e_prop),
				o_prop ? fdt32_to_cpu(*o_prop) : 0, &dt_phys);
		else
			pr_warn("goyawifi: no initramfs in FDT (FDT at %pa)\n",
				&dt_phys);
		return;
	}

	size = fdt32_to_cpu(*size_prop);
	total = fdt_totalsize(blob);
	if (!size || size > SZ_16M ||
	    total < size + SZ_4K + sizeof(struct fdt_header))
		return;

	dt_size = total - size - SZ_4K;
	if (fdt_off_dt_strings(blob) + fdt_size_dt_strings(blob) > dt_size ||
	    fdt_off_dt_struct(blob) + fdt_size_dt_struct(blob) > dt_size)
		return;

	end64 = (u64)dt_phys + total;
	if (end64 > U32_MAX)
		return;

	end = (u32)end64;
	start = end - size;
	if (fdt_setprop_inplace_u32(blob, chosen, "linux,initrd-start",
				    start) ||
	    fdt_setprop_inplace_u32(blob, chosen, "linux,initrd-end", end))
		return;

	fdt_set_totalsize(blob, dt_size);

	pr_info("goyawifi: external initramfs %08x..%08x, FDT at %pa\n",
		start, end, &dt_phys);
}
