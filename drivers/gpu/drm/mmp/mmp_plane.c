// SPDX-License-Identifier: GPL-2.0-only
/*
 * Marvell PXA988/MMP atomic DRM plane (GRA primary & DMA video overlay).
 */

#include <linux/dma-mapping.h>
#include <linux/io.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_blend.h>
#include <drm/drm_color_mgmt.h>
#include <drm/drm_fb_cma_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_gem_cma_helper.h>
#include <drm/drm_plane_helper.h>

#include "mmp_drv.h"
#include "mmp_regs.h"

struct mmp_format_info {
	u32 fourcc;
	u8 hw_fmt;
	bool swaprb;
	bool swapuv;
	bool swapyu;
	bool yuv2rgb;
	bool semiplanar;
};

static const struct mmp_format_info mmp_formats[] = {
	{ DRM_FORMAT_XRGB8888,  3, true,  false, false, false, false },
	{ DRM_FORMAT_XBGR8888,  3, false, false, false, false, false },
	{ DRM_FORMAT_ARGB8888,  4, true,  false, false, false, false },
	{ DRM_FORMAT_ABGR8888,  4, false, false, false, false, false },
	{ DRM_FORMAT_RGB888,    2, true,  false, false, false, false },
	{ DRM_FORMAT_BGR888,    2, false, false, false, false, false },
	{ DRM_FORMAT_XRGB1555,  1, true,  false, false, false, false },
	{ DRM_FORMAT_XBGR1555,  1, false, false, false, false, false },
	{ DRM_FORMAT_ARGB1555,  1, true,  false, false, false, false },
	{ DRM_FORMAT_ABGR1555,  1, false, false, false, false, false },
	{ DRM_FORMAT_RGB565,    0, true,  false, false, false, false },
	{ DRM_FORMAT_BGR565,    0, false, false, false, false, false },
	{ DRM_FORMAT_UYVY,      5, false, false, false, true,  false },
	{ DRM_FORMAT_VYUY,      5, false, true,  false, true,  false },
	{ DRM_FORMAT_YUYV,      5, false, false, true,  true,  false },
	{ DRM_FORMAT_YVYU,      5, false, true,  true,  true,  false },
	{ DRM_FORMAT_YUV422,    6, false, false, false, true,  false },
	{ DRM_FORMAT_YVU422,    6, false, true,  false, true,  false },
	{ DRM_FORMAT_YUV420,    7, false, false, false, true,  false },
	{ DRM_FORMAT_YVU420,    7, false, true,  false, true,  false },
	{ DRM_FORMAT_NV12,     12, false, false, false, true,  true  },
	{ DRM_FORMAT_NV21,     12, false, true,  false, true,  true  },
};

static const u32 mmp_primary_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
	DRM_FORMAT_XBGR8888,
	DRM_FORMAT_ABGR8888,
	DRM_FORMAT_RGB888,
	DRM_FORMAT_BGR888,
	DRM_FORMAT_XRGB1555,
	DRM_FORMAT_ARGB1555,
	DRM_FORMAT_XBGR1555,
	DRM_FORMAT_ABGR1555,
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
	DRM_FORMAT_XRGB1555,
	DRM_FORMAT_ARGB1555,
	DRM_FORMAT_XBGR1555,
	DRM_FORMAT_ABGR1555,
	DRM_FORMAT_RGB565,
	DRM_FORMAT_BGR565,
	DRM_FORMAT_UYVY,
	DRM_FORMAT_VYUY,
	DRM_FORMAT_YUYV,
	DRM_FORMAT_YVYU,
	DRM_FORMAT_YUV422,
	DRM_FORMAT_YVU422,
	DRM_FORMAT_YUV420,
	DRM_FORMAT_YVU420,
	DRM_FORMAT_NV12,
	DRM_FORMAT_NV21,
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
	u32 hsmooth_bit;
	u32 swaprb_bit;
	u32 swapuv_bit;
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
	.hsmooth_bit = CFG_GRA_HSMOOTH,
	.swaprb_bit = CFG_GRA_SWAPRB,
	.swapuv_bit = CFG_GRA_SWAPUV,
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
	.hsmooth_bit = CFG_DMA_HSMOOTH,
	.swaprb_bit = CFG_DMA_SWAPRB,
	.swapuv_bit = CFG_DMA_SWAPUV,
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

static dma_addr_t mmp_plane_fb_addr(const struct drm_plane_state *state,
				    unsigned int plane)
{
	struct drm_framebuffer *fb = state->fb;
	const struct drm_format_info *info = fb->format;
	struct drm_gem_cma_object *obj = drm_fb_cma_get_gem_obj(fb, plane);
	u32 src_x = (state->src.x1 >> 16) & ~(info->hsub - 1);
	u32 src_y = (state->src.y1 >> 16) & ~(info->vsub - 1);

	if (!obj)
		return 0;

	if (plane > 0) {
		src_x /= info->hsub;
		src_y /= info->vsub;
	}

	return obj->paddr + fb->offsets[plane] +
	       src_y * fb->pitches[plane] + src_x * info->cpp[plane];
}

static int mmp_plane_atomic_check(struct drm_plane *plane,
				  struct drm_plane_state *state)
{
	struct mmp_plane *mplane = to_mmp_plane(plane);
	struct drm_crtc_state *crtc_state = NULL;
	const struct mmp_format_info *fmt;
	u32 src_w, src_h, dst_w, dst_h;
	int ret;

	if (state->crtc) {
		crtc_state = drm_atomic_get_new_crtc_state(state->state,
							   state->crtc);
		if (!crtc_state)
			return -EINVAL;
	}

	ret = drm_atomic_helper_check_plane_state(state, crtc_state,
						  0, INT_MAX, true, true);
	if (ret || !state->visible)
		return ret;

	fmt = mmp_lookup_format(state->fb->format->format);
	if (!fmt || (!mplane->is_overlay && fmt->yuv2rgb))
		return -EINVAL;

	src_w = drm_rect_width(&state->src) >> 16;
	src_h = drm_rect_height(&state->src) >> 16;
	dst_w = drm_rect_width(&state->dst);
	dst_h = drm_rect_height(&state->dst);

	if (!src_w || !src_h || !dst_w || !dst_h ||
	    src_w > 0xfff || src_h > 0xfff || dst_w > 0xfff || dst_h > 0xfff)
		return -ERANGE;

	return 0;
}

static void mmp_plane_program_yuv(const struct drm_plane_state *state,
				  struct mmp_plane *mplane,
				  const struct mmp_format_info *fmt)
{
	const struct drm_framebuffer *fb = state->fb;
	unsigned int num_planes = fb->format->num_planes;
	u32 csc, yuvsp;

	if (num_planes == 3) {
		unsigned int u = fmt->swapuv ? 2 : 1;
		unsigned int v = fmt->swapuv ? 1 : 2;
		dma_addr_t u_addr = mmp_plane_fb_addr(state, u);
		dma_addr_t v_addr = mmp_plane_fb_addr(state, v);

		writel((u32)u_addr, mplane->regs + LCD_SPU_DMA_START_ADDR_U0);
		writel((u32)u_addr, mplane->regs + LCD_SPU_DMA_START_ADDR_U1);
		writel((u32)v_addr, mplane->regs + LCD_SPU_DMA_START_ADDR_V0);
		writel((u32)v_addr, mplane->regs + LCD_SPU_DMA_START_ADDR_V1);
		writel(((fb->pitches[v] & 0xffff) << 16) |
		       (fb->pitches[u] & 0xffff),
		       mplane->regs + LCD_SPU_DMA_PITCH_UV);
	} else if (num_planes == 2) {
		dma_addr_t uv_addr = mmp_plane_fb_addr(state, 1);
		u32 uv_pitch = fb->pitches[1] & 0xffff;

		writel((u32)uv_addr, mplane->regs + LCD_SPU_DMA_START_ADDR_U0);
		writel((u32)uv_addr, mplane->regs + LCD_SPU_DMA_START_ADDR_U1);
		writel((u32)uv_addr, mplane->regs + LCD_SPU_DMA_START_ADDR_V0);
		writel((u32)uv_addr, mplane->regs + LCD_SPU_DMA_START_ADDR_V1);
		writel((uv_pitch << 16) | uv_pitch,
		       mplane->regs + LCD_SPU_DMA_PITCH_UV);
	}

	csc = readl(mplane->regs + LCD_SPU_IOPAD_CONTROL);
	csc &= ~CFG_CSC_MASK;
	csc |= CFG_CSC_RGB_COMPUTER |
	       (state->color_encoding == DRM_COLOR_YCBCR_BT709 ?
		CFG_CSC_YUV_CCIR709 : CFG_CSC_YUV_CCIR601);
	writel(csc, mplane->regs + LCD_SPU_IOPAD_CONTROL);

	if (fmt->semiplanar) {
		yuvsp = readl(mplane->regs + TIMING_MASTER_CONTROL);
		if (fmt->swapuv)
			yuvsp |= YUV420SP_SWAPUV_PN;
		else
			yuvsp &= ~YUV420SP_SWAPUV_PN;
		writel(yuvsp, mplane->regs + TIMING_MASTER_CONTROL);
	}
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

	paddr = mmp_plane_fb_addr(state, 0);
	src_w = drm_rect_width(&state->src) >> 16;
	src_h = drm_rect_height(&state->src) >> 16;
	dst_w = drm_rect_width(&state->dst);
	dst_h = drm_rect_height(&state->dst);

	/* Ensure framebuffer writes hit RAM before LCDC DMA reads them. */
	dma_wmb();

	writel((u32)paddr, mplane->regs + lregs->start0);
	writel((u32)paddr, mplane->regs + lregs->start1);
	writel(state->fb->pitches[0] & 0xffff, mplane->regs + lregs->pitch);
	if (mplane->is_overlay && fmt->yuv2rgb)
		mmp_plane_program_yuv(state, mplane, fmt);
	writel(((state->dst.y1 & 0xfff) << 16) | (state->dst.x1 & 0xfff),
	       mplane->regs + lregs->ovsa);
	writel(((src_h & 0xfff) << 16) | (src_w & 0xfff),
	       mplane->regs + lregs->hpxl);
	writel(((dst_h & 0xfff) << 16) | (dst_w & 0xfff),
	       mplane->regs + lregs->zm);

	if (mplane->is_overlay) {
		u32 afa = readl(mplane->regs + LCD_AFA_ALL2ONE);

		if (!(src_w & 0x1) && ((src_w >> 1) >= dst_w))
			afa |= AFA_DMA_TWO_ZOOM_PN;
		else
			afa &= ~AFA_DMA_TWO_ZOOM_PN;
		writel(afa, mplane->regs + LCD_AFA_ALL2ONE);
	}

	ctrl0 = readl(mplane->regs + LCD_SPU_DMA_CTRL0);
	ctrl0 &= ~lregs->ctrl0_mask;
	ctrl0 |= CFG_ARBFAST_ENA | lregs->ena_bit |
		 ((u32)fmt->hw_fmt << lregs->fmt_shift);
	if (fmt->swaprb)
		ctrl0 |= lregs->swaprb_bit;
	if (fmt->swapuv && state->fb->format->num_planes == 1)
		ctrl0 |= lregs->swapuv_bit;
	if (fmt->swapyu)
		ctrl0 |= lregs->swapyu_bit;
	if (fmt->yuv2rgb)
		ctrl0 |= lregs->yuv2rgb_bit;
	if (src_w != dst_w)
		ctrl0 |= lregs->hsmooth_bit;
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

	if (mplane->is_overlay) {
		u32 afa = readl(mplane->regs + LCD_AFA_ALL2ONE);

		afa &= ~AFA_DMA_TWO_ZOOM_PN;
		writel(afa, mplane->regs + LCD_AFA_ALL2ONE);
	}
}

static void mmp_plane_reset(struct drm_plane *plane)
{
	struct mmp_plane *mplane = to_mmp_plane(plane);

	drm_atomic_helper_plane_reset(plane);
	if (!plane->state)
		return;

	plane->state->zpos = mplane->is_overlay ? 0 : 1;
	plane->state->normalized_zpos = plane->state->zpos;
	if (plane->color_encoding_property) {
		plane->state->color_encoding = DRM_COLOR_YCBCR_BT601;
		plane->state->color_range = DRM_COLOR_YCBCR_LIMITED_RANGE;
	}
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
	.reset = mmp_plane_reset,
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
	ret = drm_plane_create_alpha_property(&mplane->base);
	if (ret)
		return ret;

	ret = drm_plane_create_zpos_immutable_property(&mplane->base,
						       is_overlay ? 0 : 1);
	if (ret)
		return ret;

	if (is_overlay) {
		u32 encodings = BIT(DRM_COLOR_YCBCR_BT601) |
				BIT(DRM_COLOR_YCBCR_BT709);
		u32 ranges = BIT(DRM_COLOR_YCBCR_LIMITED_RANGE);

		ret = drm_plane_create_color_properties(&mplane->base,
							encodings, ranges,
							DRM_COLOR_YCBCR_BT601,
							DRM_COLOR_YCBCR_LIMITED_RANGE);
		if (ret)
			return ret;
	}

	return 0;
}
