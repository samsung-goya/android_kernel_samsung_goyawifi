// SPDX-License-Identifier: GPL-2.0-only
/*
 * Marvell PXA988/MMP atomic DRM plane (GRA primary & DMA video overlay).
 */

#include <linux/dma-mapping.h>
#include <linux/io.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_fb_cma_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_plane_helper.h>

#include "mmp_drv.h"
#include "mmp_regs.h"

struct mmp_format_info {
	u32 fourcc;
	u8 hw_fmt;
	bool swaprb;
	bool swapyu;
	bool yuv2rgb;
};

static const struct mmp_format_info mmp_formats[] = {
	{ DRM_FORMAT_XRGB8888, 4, true,  false, false },
	{ DRM_FORMAT_ARGB8888, 4, true,  false, false },
	{ DRM_FORMAT_XBGR8888, 4, false, false, false },
	{ DRM_FORMAT_ABGR8888, 4, false, false, false },
	{ DRM_FORMAT_RGB888,   2, true,  false, false },
	{ DRM_FORMAT_BGR888,   2, false, false, false },
	{ DRM_FORMAT_RGB565,   0, true,  false, false },
	{ DRM_FORMAT_BGR565,   0, false, false, false },
	{ DRM_FORMAT_UYVY,     5, false, false, true  },
	{ DRM_FORMAT_YUYV,     5, false, true,  true  },
};

static const u32 mmp_primary_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
	DRM_FORMAT_XBGR8888,
	DRM_FORMAT_ABGR8888,
	DRM_FORMAT_RGB888,
	DRM_FORMAT_BGR888,
	DRM_FORMAT_RGB565,
	DRM_FORMAT_BGR565,
};

static const u32 mmp_overlay_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
	DRM_FORMAT_XBGR8888,
	DRM_FORMAT_ABGR8888,
	DRM_FORMAT_RGB888,
	DRM_FORMAT_BGR888,
	DRM_FORMAT_RGB565,
	DRM_FORMAT_BGR565,
	DRM_FORMAT_UYVY,
	DRM_FORMAT_YUYV,
};

struct mmp_layer_regs {
	u32 start0;
	u32 start1;
	u32 pitch;
	u32 ovsa;
	u32 hpxl;
	u32 zm;
	u32 ctrl0_mask;
	u32 ena_bit;
	u32 swaprb_bit;
	u32 swapyu_bit;
	u32 yuv2rgb_bit;
	u8 fmt_shift;
};

static const struct mmp_layer_regs mmp_gra_regs = {
	.start0 = LCD_CFG_GRA_START_ADDR0,
	.start1 = LCD_CFG_GRA_START_ADDR1,
	.pitch = LCD_CFG_GRA_PITCH,
	.ovsa = LCD_SPU_GRA_OVSA_HPXL_VLN,
	.hpxl = LCD_SPU_GRA_HPXL_VLN,
	.zm = LCD_SPU_GZM_HPXL_VLN,
	.ctrl0_mask = CFG_GRA_ALL_MASK,
	.ena_bit = CFG_GRA_ENA,
	.swaprb_bit = CFG_GRA_SWAPRB,
	.swapyu_bit = CFG_GRA_SWAPYU,
	.yuv2rgb_bit = CFG_YUV2RGB_GRA,
	.fmt_shift = 16,
};

static const struct mmp_layer_regs mmp_dma_regs = {
	.start0 = LCD_SPU_DMA_START_ADDR_Y0,
	.start1 = LCD_SPU_DMA_START_ADDR_Y1,
	.pitch = LCD_SPU_DMA_PITCH_YC,
	.ovsa = LCD_SPU_DMA_OVSA_HPXL_VLN,
	.hpxl = LCD_SPU_DMA_HPXL_VLN,
	.zm = LCD_SPU_DZM_HPXL_VLN,
	.ctrl0_mask = CFG_DMA_ALL_MASK,
	.ena_bit = CFG_DMA_ENA,
	.swaprb_bit = CFG_DMA_SWAPRB,
	.swapyu_bit = CFG_DMA_SWAPYU,
	.yuv2rgb_bit = CFG_YUV2RGB_DMA,
	.fmt_shift = 20,
};

static const struct mmp_format_info *mmp_lookup_format(u32 fourcc)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(mmp_formats); i++) {
		if (mmp_formats[i].fourcc == fourcc)
			return &mmp_formats[i];
	}

	return NULL;
}

static inline const struct mmp_layer_regs *
mmp_plane_layer_regs(const struct mmp_plane *mplane)
{
	return mplane->is_overlay ? &mmp_dma_regs : &mmp_gra_regs;
}

static int mmp_plane_atomic_check(struct drm_plane *plane,
				  struct drm_plane_state *state)
{
	struct mmp_plane *mplane = to_mmp_plane(plane);
	struct drm_crtc_state *crtc_state = NULL;

	if (state->crtc) {
		crtc_state = drm_atomic_get_new_crtc_state(state->state,
							   state->crtc);
		if (!crtc_state)
			return -EINVAL;
	}

	if (state->fb && !mmp_lookup_format(state->fb->format->format))
		return -EINVAL;

	return drm_atomic_helper_check_plane_state(state, crtc_state,
						   DRM_PLANE_HELPER_NO_SCALING,
						   DRM_PLANE_HELPER_NO_SCALING,
						   mplane->is_overlay, true);
}

static void mmp_plane_atomic_update(struct drm_plane *plane,
				    struct drm_plane_state *old_state)
{
	struct mmp_plane *mplane = to_mmp_plane(plane);
	struct drm_plane_state *state = plane->state;
	const struct mmp_layer_regs *lregs = mmp_plane_layer_regs(mplane);
	const struct mmp_format_info *fmt;
	u32 src_w, src_h, dst_w, dst_h;
	dma_addr_t paddr;
	u32 ctrl0;

	if (!state->crtc || !state->fb || !state->visible)
		return;

	fmt = mmp_lookup_format(state->fb->format->format);
	if (!fmt)
		return;

	paddr = drm_fb_cma_get_gem_addr(state->fb, state, 0);
	src_w = drm_rect_width(&state->src) >> 16;
	src_h = drm_rect_height(&state->src) >> 16;
	dst_w = drm_rect_width(&state->dst);
	dst_h = drm_rect_height(&state->dst);

	/* Ensure framebuffer writes hit RAM before LCDC DMA reads them. */
	dma_wmb();

	writel((u32)paddr, mplane->regs + lregs->start0);
	writel((u32)paddr, mplane->regs + lregs->start1);
	writel(state->fb->pitches[0] & 0xffff, mplane->regs + lregs->pitch);
	writel(((state->dst.y1 & 0xfff) << 16) | (state->dst.x1 & 0xfff),
	       mplane->regs + lregs->ovsa);
	writel(((src_h & 0xfff) << 16) | (src_w & 0xfff),
	       mplane->regs + lregs->hpxl);
	writel(((dst_h & 0xfff) << 16) | (dst_w & 0xfff),
	       mplane->regs + lregs->zm);

	ctrl0 = readl(mplane->regs + LCD_SPU_DMA_CTRL0);
	ctrl0 &= ~lregs->ctrl0_mask;
	ctrl0 |= CFG_ARBFAST_ENA | lregs->ena_bit |
		 ((u32)fmt->hw_fmt << lregs->fmt_shift);
	if (fmt->swaprb)
		ctrl0 |= lregs->swaprb_bit;
	if (fmt->swapyu)
		ctrl0 |= lregs->swapyu_bit;
	if (fmt->yuv2rgb)
		ctrl0 |= lregs->yuv2rgb_bit;
	writel(ctrl0, mplane->regs + LCD_SPU_DMA_CTRL0);
}

static void mmp_plane_atomic_disable(struct drm_plane *plane,
				     struct drm_plane_state *old_state)
{
	struct mmp_plane *mplane = to_mmp_plane(plane);
	const struct mmp_layer_regs *lregs = mmp_plane_layer_regs(mplane);
	u32 ctrl0;

	ctrl0 = readl(mplane->regs + LCD_SPU_DMA_CTRL0);
	ctrl0 &= ~lregs->ena_bit;
	writel(ctrl0, mplane->regs + LCD_SPU_DMA_CTRL0);
}

static const struct drm_plane_helper_funcs mmp_plane_helper_funcs = {
	.atomic_check = mmp_plane_atomic_check,
	.atomic_update = mmp_plane_atomic_update,
	.atomic_disable = mmp_plane_atomic_disable,
};

static const struct drm_plane_funcs mmp_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	.reset = drm_atomic_helper_plane_reset,
	.atomic_duplicate_state = drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_plane_destroy_state,
};

int mmp_plane_init(struct drm_device *drm, struct mmp_plane *mplane,
		   void __iomem *regs, unsigned int possible_crtcs,
		   bool is_overlay)
{
	const u32 *formats = is_overlay ? mmp_overlay_formats :
					  mmp_primary_formats;
	unsigned int num_formats = is_overlay ? ARRAY_SIZE(mmp_overlay_formats) :
						ARRAY_SIZE(mmp_primary_formats);
	enum drm_plane_type type = is_overlay ? DRM_PLANE_TYPE_OVERLAY :
						DRM_PLANE_TYPE_PRIMARY;
	int ret;

	mplane->regs = regs;
	mplane->is_overlay = is_overlay;

	ret = drm_universal_plane_init(drm, &mplane->base, possible_crtcs,
				       &mmp_plane_funcs, formats, num_formats,
				       NULL, type, NULL);
	if (ret)
		return ret;

	drm_plane_helper_add(&mplane->base, &mmp_plane_helper_funcs);
	return 0;
}
