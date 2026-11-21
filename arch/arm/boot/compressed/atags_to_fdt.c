// SPDX-License-Identifier: GPL-2.0
#include <asm/setup.h>
#include <libfdt.h>

#if defined(CONFIG_ARM_ATAG_DTB_COMPAT_CMDLINE_EXTEND)
#define do_extend_cmdline 1
#else
#define do_extend_cmdline 0
#endif

#define NR_BANKS 16

#ifdef CONFIG_ARM_SAMSUNG_PXA_BOOTFIXUP
/*
 * Samsung SM-T110 (goyawifi) S-Boot passes a fixed 16 MiB ATAG_INITRD2 span
 * that may sit in low RAM where head.S zImage relocation or kernel
 * decompression would overwrite it. Relocate low initrd payloads into free
 * lowmem (0x0c000000..0x0cffffff, clear of OBM, CP/ramoops at 0x06000000..
 * 0x0817ffff, 0x10008000, framebuffer at 0x17000000, and CMA at 0x20000000)
 * before head.S relocates zImage. Keep /chosen/bootargs and /memory@0 from
 * the appended DTB.
 */
#define GOYAWIFI_INITRD_LOW_LIMIT	0x03000000U
#define GOYAWIFI_INITRD_SAFE_ADDR	0x0c000000U
#define GOYAWIFI_INITRD_MAX_SIZE	0x01000000U

static uint32_t goya_parse_u32(const char **pp)
{
	const char *p = *pp;
	uint32_t val = 0;

	if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
		p += 2;
		while (1) {
			if (*p >= '0' && *p <= '9')
				val = (val << 4) | (*p - '0');
			else if (*p >= 'a' && *p <= 'f')
				val = (val << 4) | (*p - 'a' + 10);
			else if (*p >= 'A' && *p <= 'F')
				val = (val << 4) | (*p - 'A' + 10);
			else
				break;
			p++;
		}
	} else {
		while (*p >= '0' && *p <= '9') {
			val = val * 10 + (*p - '0');
			p++;
		}
	}

	if (*p == 'k' || *p == 'K') {
		val <<= 10;
		p++;
	} else if (*p == 'm' || *p == 'M') {
		val <<= 20;
		p++;
	}

	*pp = p;
	return val;
}

static int goya_parse_cmdline_initrd(const char *cmdline,
				     uint32_t *start_out,
				     uint32_t *size_out)
{
	const char *p = cmdline;

	while (*p) {
		if ((p == cmdline || p[-1] == ' ' || p[-1] == '\t') &&
		    p[0] == 'i' && p[1] == 'n' && p[2] == 'i' &&
		    p[3] == 't' && p[4] == 'r' && p[5] == 'd' && p[6] == '=') {
			const char *q = p + 7;
			uint32_t start = goya_parse_u32(&q);

			if (*q == ',') {
				uint32_t size;

				q++;
				size = goya_parse_u32(&q);
				if (size) {
					*start_out = start;
					*size_out = size;
					return 1;
				}
			}
		}
		p++;
	}
	return 0;
}

static int goya_cmdline_has_token(const char *cmdline, const char *token)
{
	size_t tlen = strlen(token);
	const char *p = cmdline;

	while (*p) {
		if ((p == cmdline || p[-1] == ' ' || p[-1] == '\t') &&
		    memcmp(p, token, tlen) == 0 &&
		    (p[tlen] == '\0' || p[tlen] == ' ' || p[tlen] == '\t'))
			return 1;
		p++;
	}
	return 0;
}
#endif

static int node_offset(void *fdt, const char *node_path)
{
	int offset = fdt_path_offset(fdt, node_path);
	if (offset == -FDT_ERR_NOTFOUND)
		offset = fdt_add_subnode(fdt, 0, node_path);
	return offset;
}

static int setprop(void *fdt, const char *node_path, const char *property,
		   uint32_t *val_array, int size)
{
	int offset = node_offset(fdt, node_path);
	if (offset < 0)
		return offset;
	return fdt_setprop(fdt, offset, property, val_array, size);
}

static int setprop_string(void *fdt, const char *node_path,
			  const char *property, const char *string)
{
	int offset = node_offset(fdt, node_path);
	if (offset < 0)
		return offset;
	return fdt_setprop_string(fdt, offset, property, string);
}

static int setprop_cell(void *fdt, const char *node_path,
			const char *property, uint32_t val)
{
	int offset = node_offset(fdt, node_path);
	if (offset < 0)
		return offset;
	return fdt_setprop_cell(fdt, offset, property, val);
}

static const void *getprop(const void *fdt, const char *node_path,
			   const char *property, int *len)
{
	int offset = fdt_path_offset(fdt, node_path);

	if (offset == -FDT_ERR_NOTFOUND)
		return NULL;

	return fdt_getprop(fdt, offset, property, len);
}

static uint32_t get_cell_size(const void *fdt)
{
	int len;
	uint32_t cell_size = 1;
	const uint32_t *size_len =  getprop(fdt, "/", "#size-cells", &len);

	if (size_len)
		cell_size = fdt32_to_cpu(*size_len);
	return cell_size;
}

static void merge_fdt_bootargs(void *fdt, const char *fdt_cmdline)
{
	char cmdline[COMMAND_LINE_SIZE];
	const char *fdt_bootargs;
	char *ptr = cmdline;
	int len = 0;

	/* copy the fdt command line into the buffer */
	fdt_bootargs = getprop(fdt, "/chosen", "bootargs", &len);
	if (fdt_bootargs)
		if (len < COMMAND_LINE_SIZE) {
			memcpy(ptr, fdt_bootargs, len);
			/* len is the length of the string
			 * including the NULL terminator */
			ptr += len - 1;
		}

	/* and append the ATAG_CMDLINE */
	if (fdt_cmdline) {
		len = strlen(fdt_cmdline);
		if (ptr - cmdline + len + 2 < COMMAND_LINE_SIZE) {
			*ptr++ = ' ';
			memcpy(ptr, fdt_cmdline, len);
			ptr += len;
		}
	}
	*ptr = '\0';

	setprop_string(fdt, "/chosen", "bootargs", cmdline);
}

/*
 * Convert and fold provided ATAGs into the provided FDT.
 *
 * REturn values:
 *    = 0 -> pretend success
 *    = 1 -> bad ATAG (may retry with another possible ATAG pointer)
 *    < 0 -> error from libfdt
 */
int atags_to_fdt(void *atag_list, void *fdt, int total_space)
{
	struct tag *atag = atag_list;
	/* In the case of 64 bits memory size, need to reserve 2 cells for
	 * address and size for each bank */
	uint32_t mem_reg_property[2 * 2 * NR_BANKS];
	int memcount = 0;
	int ret, memsize;
#ifdef CONFIG_ARM_SAMSUNG_PXA_BOOTFIXUP
	int is_goyawifi = 0;
	uint32_t goya_initrd_start = 0, goya_initrd_size = 0;
#endif

	/* make sure we've got an aligned pointer */
	if ((u32)atag_list & 0x3)
		return 1;

	/* if we get a DTB here we're done already */
	if (*(u32 *)atag_list == fdt32_to_cpu(FDT_MAGIC))
	       return 0;

	/* validate the ATAG */
	if (atag->hdr.tag != ATAG_CORE ||
	    (atag->hdr.size != tag_size(tag_core) &&
	     atag->hdr.size != 2))
		return 1;

#ifdef CONFIG_ARM_SAMSUNG_PXA_BOOTFIXUP
	if (fdt_node_check_compatible(fdt, 0, "samsung,goyawifi") == 0) {
		int len;

		if (getprop(fdt, "/chosen", "samsung,goyawifi-initrd-size", &len))
			return 0;
		is_goyawifi = 1;
	}
#endif

	/* let's give it all the room it could need */
	ret = fdt_open_into(fdt, fdt, total_space);
	if (ret < 0)
		return ret;

	for_each_tag(atag, atag_list) {
		if (atag->hdr.tag == ATAG_CMDLINE) {
#ifdef CONFIG_ARM_SAMSUNG_PXA_BOOTFIXUP
			if (is_goyawifi) {
				setprop_string(fdt, "/chosen",
					       "samsung,goyawifi-boot-cmdline",
					       atag->u.cmdline.cmdline);
				if (goya_cmdline_has_token(atag->u.cmdline.cmdline,
							   "lpcharge=1") ||
				    goya_cmdline_has_token(atag->u.cmdline.cmdline,
							   "androidboot.mode=charger") ||
				    goya_cmdline_has_token(atag->u.cmdline.cmdline,
							   "lpm_boot=1"))
					merge_fdt_bootargs(fdt,
							   "androidboot.mode=charger lpcharge=1");
				if (!goya_initrd_size)
					goya_parse_cmdline_initrd(
						atag->u.cmdline.cmdline,
						&goya_initrd_start,
						&goya_initrd_size);
				continue;
			}
#endif
			/* Append the ATAGS command line to the device tree
			 * command line.
			 * NB: This means that if the same parameter is set in
			 * the device tree and in the tags, the one from the
			 * tags will be chosen.
			 */
			if (do_extend_cmdline)
				merge_fdt_bootargs(fdt,
						   atag->u.cmdline.cmdline);
			else
				setprop_string(fdt, "/chosen", "bootargs",
					       atag->u.cmdline.cmdline);
		} else if (atag->hdr.tag == ATAG_MEM) {
#ifdef CONFIG_ARM_SAMSUNG_PXA_BOOTFIXUP
			if (is_goyawifi)
				continue;
#endif
			if (memcount >= sizeof(mem_reg_property)/4)
				continue;
			if (!atag->u.mem.size)
				continue;
			memsize = get_cell_size(fdt);

			if (memsize == 2) {
				/* if memsize is 2, that means that
				 * each data needs 2 cells of 32 bits,
				 * so the data are 64 bits */
				uint64_t *mem_reg_prop64 =
					(uint64_t *)mem_reg_property;
				mem_reg_prop64[memcount++] =
					cpu_to_fdt64(atag->u.mem.start);
				mem_reg_prop64[memcount++] =
					cpu_to_fdt64(atag->u.mem.size);
			} else {
				mem_reg_property[memcount++] =
					cpu_to_fdt32(atag->u.mem.start);
				mem_reg_property[memcount++] =
					cpu_to_fdt32(atag->u.mem.size);
			}

		} else if (atag->hdr.tag == ATAG_INITRD2) {
			uint32_t initrd_start, initrd_size;
			initrd_start = atag->u.initrd.start;
			initrd_size = atag->u.initrd.size;
#ifdef CONFIG_ARM_SAMSUNG_PXA_BOOTFIXUP
			if (is_goyawifi) {
				if (initrd_size) {
					goya_initrd_start = initrd_start;
					goya_initrd_size = initrd_size;
				}
				continue;
			}
#endif
			setprop_cell(fdt, "/chosen", "linux,initrd-start",
					initrd_start);
			setprop_cell(fdt, "/chosen", "linux,initrd-end",
					initrd_start + initrd_size);
		}
#ifdef CONFIG_ARM_SAMSUNG_PXA_BOOTFIXUP
		else if (is_goyawifi && atag->hdr.tag == ATAG_INITRD) {
			if (atag->u.initrd.size && !goya_initrd_size) {
				goya_initrd_start = atag->u.initrd.start;
				if (goya_initrd_start >= 0xc0000000U)
					goya_initrd_start -= 0xc0000000U;
				goya_initrd_size = atag->u.initrd.size;
			}
		}
#endif
	}

#ifdef CONFIG_ARM_SAMSUNG_PXA_BOOTFIXUP
	if (is_goyawifi && goya_initrd_size) {
		uint32_t orig_start = goya_initrd_start;

		if (goya_initrd_size > GOYAWIFI_INITRD_MAX_SIZE)
			goya_initrd_size = GOYAWIFI_INITRD_MAX_SIZE;
		if (goya_initrd_start < GOYAWIFI_INITRD_LOW_LIMIT) {
			memcpy((void *)GOYAWIFI_INITRD_SAFE_ADDR,
			       (const void *)goya_initrd_start,
			       goya_initrd_size);
			goya_initrd_start = GOYAWIFI_INITRD_SAFE_ADDR;
		}
		setprop_cell(fdt, "/chosen", "samsung,goyawifi-orig-initrd",
			     orig_start);
		setprop_cell(fdt, "/chosen", "linux,initrd-start",
			     goya_initrd_start);
		setprop_cell(fdt, "/chosen", "linux,initrd-end",
			     goya_initrd_start + goya_initrd_size);
	}
#endif

	if (memcount) {
		setprop(fdt, "/memory", "reg", mem_reg_property,
			4 * memcount * memsize);
	}

	return fdt_pack(fdt);
}
