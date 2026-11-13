// SPDX-License-Identifier: GPL-2.0-only
/*
 * Marvell PXA988/MMP CRTC and display timing controller driver.
 */

#include <linux/clk.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/of.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_color_mgmt.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_vblank.h>

#include "mmp_drv.h"
#include "mmp_regs.h"

#define MMP_GAMMA_LUT_SIZE	256

static int mmp_crtc_clks_enable(struct mmp_crtc *mcrtc)
{
	int ret;

	if (mcrtc->clks_on)
		return 0;

	ret = clk_prepare_enable(mcrtc->clk_hclk);
	if (ret)
		return ret;

	ret = clk_prepare_enable(mcrtc->clk_axi);
	if (ret)
		goto err_hclk;

	ret = clk_prepare_enable(mcrtc->clk_lcd);
	if (ret)
		goto err_axi;

	if (mcrtc->takeover && mcrtc->clk_dsi_phy) {
		ret = clk_prepare_enable(mcrtc->clk_dsi_phy);
		if (ret)
			goto err_lcd;
	}

	if (mcrtc->takeover && mcrtc->clk_dsi_esc) {
		ret = clk_prepare_enable(mcrtc->clk_dsi_esc);
		if (ret)
			goto err_dsi_phy;
	}

	mcrtc->clks_on = true;
	return 0;

err_dsi_phy:
	if (mcrtc->takeover && mcrtc->clk_dsi_phy)
		clk_disable_unprepare(mcrtc->clk_dsi_phy);
err_lcd:
	clk_disable_unprepare(mcrtc->clk_lcd);
err_axi:
	clk_disable_unprepare(mcrtc->clk_axi);
err_hclk:
	clk_disable_unprepare(mcrtc->clk_hclk);
	return ret;
}

static void mmp_crtc_clks_disable(struct mmp_crtc *mcrtc)
{
	if (!mcrtc->clks_on)
		return;

	if (mcrtc->takeover && mcrtc->clk_dsi_esc)
		clk_disable_unprepare(mcrtc->clk_dsi_esc);
	if (mcrtc->takeover && mcrtc->clk_dsi_phy)
		clk_disable_unprepare(mcrtc->clk_dsi_phy);
	clk_disable_unprepare(mcrtc->clk_lcd);
	clk_disable_unprepare(mcrtc->clk_axi);
	clk_disable_unprepare(mcrtc->clk_hclk);

	mcrtc->clks_on = false;
}

static int mmp_crtc_atomic_check(struct drm_crtc *crtc,
				 struct drm_crtc_state *state)
{
	if (state->enable && (state->mode.flags & DRM_MODE_FLAG_INTERLACE))
		return -EINVAL;

	if (state->gamma_lut &&
	    state->gamma_lut->length != MMP_GAMMA_LUT_SIZE *
					sizeof(struct drm_color_lut))
		return -EINVAL;

	return 0;
}

static void mmp_crtc_mode_set_nofb(struct drm_crtc *crtc)
{
	struct mmp_crtc *mcrtc = to_mmp_crtc(crtc);
	const struct drm_display_mode *mode = &crtc->state->adjusted_mode;
	u32 hfp, hbp, htot_hw, hbp_hw, hfp_hw;
	u32 vfp, vbp, master;
	int ret;

	/*
	 * In M1 bootloader-takeover mode the active DSI/VX5B3D pipeline is
	 * kept alive as configured by the bootloader; touching SCLK or timing
	 * registers risks breaking link lock until DSI host control lands.
	 */
	if (mcrtc->takeover)
		return;

	ret = mmp_crtc_clks_enable(mcrtc);
	if (ret) {
		dev_err(crtc->dev->dev, "failed to enable lcdc clocks: %d\n", ret);
		return;
	}

	hfp = mode->hsync_start - mode->hdisplay;
	hbp = mode->htotal - mode->hsync_end;
	htot_hw = (mode->htotal * 3) / 2;
	hbp_hw = (hbp * 3) / 2;
	hfp_hw = ((mode->hdisplay + hfp) * 3) / 2 - mode->hdisplay;

	vfp = mode->vsync_start - mode->vdisplay;
	vbp = mode->vtotal - mode->vsync_end;

	writel(0x0000fff0, mcrtc->regs + LCD_TOP_CTRL);
	writel(0, mcrtc->regs + LCD_SPU_SRAM_CTRL);
	writel(0, mcrtc->regs + LCD_AFA_ALL2ONE);
	writel(0, mcrtc->regs + LCD_SQULN1_CTRL);
	writel(0, mcrtc->regs + LCD_SQULN2_CTRL);
	writel(CFG_CYC_BURST_LEN16, mcrtc->regs + LCD_SPU_IOPAD_CONTROL);
	writel(0, mcrtc->regs + LCD_SPU_BLANKCOLOR);
	writel(0x28320081, mcrtc->regs + LCD_SPU_DMA_CTRL1);

	writel((mode->vtotal << 16) | htot_hw, mcrtc->regs + LCD_SPU_V_H_TOTAL);
	writel((mode->vdisplay << 16) | mode->hdisplay,
	       mcrtc->regs + LCD_SPU_V_H_ACTIVE);
	writel((hbp_hw << 16) | hfp_hw, mcrtc->regs + LCD_SPU_H_PORCH);
	writel((vbp << 16) | vfp, mcrtc->regs + LCD_SPU_V_PORCH);
	writel(0x01330133, mcrtc->regs + LCD_PN_SEPXLCNT);

	/* 624 MHz PLL1_624 / 2 = 312 MHz DSI bitclk, / 8 = 78 MHz PN pixclk */
	writel(SCLK_SRC_DISP1 | SCLK_DSI_SEL | SCLK_DSI1_DIV(2) | SCLK_INT_DIV(8),
	       mcrtc->regs + LCD_CFG_SCLK_DIV);

	master = readl(mcrtc->regs + TIMING_MASTER_CONTROL);
	master &= ~0x350;
	master |= MASTER_ENH(0) | MASTER_ENV(0);
	writel(master, mcrtc->regs + TIMING_MASTER_CONTROL);
}

static void mmp_crtc_atomic_enable(struct drm_crtc *crtc,
				   struct drm_crtc_state *old_state)
{
	struct mmp_crtc *mcrtc = to_mmp_crtc(crtc);
	int ret;

	ret = mmp_crtc_clks_enable(mcrtc);
	if (ret) {
		dev_err(crtc->dev->dev, "failed to enable lcdc clocks: %d\n", ret);
		return;
	}

	if (!mcrtc->takeover) {
		u32 sclk = readl(mcrtc->regs + LCD_CFG_SCLK_DIV);
		u32 dumb = readl(mcrtc->regs + LCD_SPU_DUMB_CTRL);

		sclk &= ~SCLK_DISABLE;
		writel(sclk, mcrtc->regs + LCD_CFG_SCLK_DIV);

		dumb &= ~(CFG_DUMBMODE_MASK | CFG_INV_PCLK |
			  CFG_INV_HSYNC | CFG_INV_VSYNC);
		dumb |= CFG_DUMB_ENA;
		writel(dumb, mcrtc->regs + LCD_SPU_DUMB_CTRL);
	}

	drm_crtc_vblank_on(crtc);
}

static void mmp_crtc_atomic_disable(struct drm_crtc *crtc,
				    struct drm_crtc_state *old_state)
{
	struct mmp_crtc *mcrtc = to_mmp_crtc(crtc);
	unsigned long flags;

	drm_crtc_vblank_off(crtc);

	spin_lock_irqsave(&crtc->dev->event_lock, flags);
	if (crtc->state->event) {
		drm_crtc_send_vblank_event(crtc, crtc->state->event);
		crtc->state->event = NULL;
	}
	spin_unlock_irqrestore(&crtc->dev->event_lock, flags);

	if (!mcrtc->takeover) {
		u32 dumb = readl(mcrtc->regs + LCD_SPU_DUMB_CTRL);
		u32 sclk = readl(mcrtc->regs + LCD_CFG_SCLK_DIV);

		dumb &= ~CFG_DUMB_ENA;
		writel(dumb, mcrtc->regs + LCD_SPU_DUMB_CTRL);

		sclk |= SCLK_DISABLE;
		writel(sclk, mcrtc->regs + LCD_CFG_SCLK_DIV);

		mmp_crtc_clks_disable(mcrtc);
	}
}

static void mmp_crtc_update_blending(struct mmp_crtc *mcrtc)
{
	struct drm_crtc *crtc = &mcrtc->base;
	struct drm_plane_state *gra_state = mcrtc->primary.base.state;
	struct drm_plane_state *dma_state = mcrtc->overlay.base.state;
	bool gra_vis = gra_state && gra_state->crtc == crtc &&
		       gra_state->visible;
	bool dma_vis = dma_state && dma_state->crtc == crtc &&
		       dma_state->visible;
	u8 gra_alpha = gra_vis ? (gra_state->alpha >> 8) : 0;
	u8 dma_alpha = dma_vis ? (dma_state->alpha >> 8) : 0;
	bool gra_px_alpha = gra_vis && gra_state->fb &&
			    gra_state->fb->format->has_alpha;
	bool dma_px_alpha = dma_vis && dma_state->fb &&
			    dma_state->fb->format->has_alpha;
	u32 ctrl1, blend;

	if (gra_vis) {
		if (gra_px_alpha && gra_alpha == 0xff)
			blend = CFG_ALPHAM_GRA | CFG_ALPHA(0x00);
		else
			blend = CFG_ALPHAM_CFG | CFG_ALPHA(255 - gra_alpha);
	} else if (dma_vis) {
		if (dma_px_alpha && dma_alpha == 0xff)
			blend = CFG_ALPHAM_VIDEO | CFG_ALPHA(0xff);
		else
			blend = CFG_ALPHAM_CFG | CFG_ALPHA(dma_alpha);
	} else {
		blend = CFG_ALPHAM_CFG | CFG_ALPHA(0x00);
	}

	ctrl1 = readl(mcrtc->regs + LCD_SPU_DMA_CTRL1);
	ctrl1 &= ~(CFG_ALPHA_MODE_MASK | CFG_ALPHA_MASK);
	ctrl1 |= blend;
	writel(ctrl1, mcrtc->regs + LCD_SPU_DMA_CTRL1);
}

static void mmp_crtc_update_gamma(struct mmp_crtc *mcrtc,
				  const struct drm_crtc_state *state)
{
	struct drm_color_lut *lut;
	u32 ctrl0, para1;
	unsigned int i;

	ctrl0 = readl(mcrtc->regs + LCD_SPU_DMA_CTRL0);
	ctrl0 &= ~CFG_GAMMA_ENA;
	writel(ctrl0, mcrtc->regs + LCD_SPU_DMA_CTRL0);

	if (!state->gamma_lut)
		return;

	lut = (struct drm_color_lut *)state->gamma_lut->data;

	para1 = readl(mcrtc->regs + LCD_SPU_SRAM_PARA1);
	para1 &= ~CFG_PDWN256x8;
	para1 |= CFG_CSB_256x8;
	writel(para1, mcrtc->regs + LCD_SPU_SRAM_PARA1);

	for (i = 0; i < MMP_GAMMA_LUT_SIZE; i++) {
		writel(drm_color_lut_extract(lut[i].red, 8),
		       mcrtc->regs + LCD_SPU_SRAM_WRDAT);
		writel(SRAM_WRITE | SRAM_ADDR_LCDID(SRAM_GAMMA_YR) | i,
		       mcrtc->regs + LCD_SPU_SRAM_CTRL);

		writel(drm_color_lut_extract(lut[i].green, 8),
		       mcrtc->regs + LCD_SPU_SRAM_WRDAT);
		writel(SRAM_WRITE | SRAM_ADDR_LCDID(SRAM_GAMMA_UG) | i,
		       mcrtc->regs + LCD_SPU_SRAM_CTRL);

		writel(drm_color_lut_extract(lut[i].blue, 8),
		       mcrtc->regs + LCD_SPU_SRAM_WRDAT);
		writel(SRAM_WRITE | SRAM_ADDR_LCDID(SRAM_GAMMA_VB) | i,
		       mcrtc->regs + LCD_SPU_SRAM_CTRL);
	}

	para1 &= ~CFG_CSB_256x8;
	writel(para1, mcrtc->regs + LCD_SPU_SRAM_PARA1);

	ctrl0 |= CFG_GAMMA_ENA;
	writel(ctrl0, mcrtc->regs + LCD_SPU_DMA_CTRL0);
}

static void mmp_crtc_atomic_flush(struct drm_crtc *crtc,
				  struct drm_crtc_state *old_state)
{
	struct mmp_crtc *mcrtc = to_mmp_crtc(crtc);
	struct drm_pending_vblank_event *event = crtc->state->event;
	unsigned long flags;

	if (crtc->state->active) {
		mmp_crtc_update_blending(mcrtc);
		if (crtc->state->color_mgmt_changed ||
		    (drm_atomic_crtc_needs_modeset(crtc->state) &&
		     crtc->state->gamma_lut))
			mmp_crtc_update_gamma(mcrtc, crtc->state);
	}

	if (!event)
		return;

	crtc->state->event = NULL;

	spin_lock_irqsave(&crtc->dev->event_lock, flags);
	if (drm_crtc_vblank_get(crtc) == 0)
		mcrtc->event = event;
	else
		drm_crtc_send_vblank_event(crtc, event);
	spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
}

static int mmp_crtc_enable_vblank(struct drm_crtc *crtc)
{
	struct mmp_crtc *mcrtc = to_mmp_crtc(crtc);
	unsigned long flags;

	spin_lock_irqsave(&mcrtc->irq_lock, flags);
	/* ISR is write-0-to-clear: clear any stale VSYNC pending bit first. */
	writel(~VSYNC_IRQ, mcrtc->regs + LCD_SPU_IRQ_ISR);
	mcrtc->irq_ena |= VSYNC_IRQ;
	writel(mcrtc->irq_ena, mcrtc->regs + LCD_SPU_IRQ_ENA);
	spin_unlock_irqrestore(&mcrtc->irq_lock, flags);

	return 0;
}

static void mmp_crtc_disable_vblank(struct drm_crtc *crtc)
{
	struct mmp_crtc *mcrtc = to_mmp_crtc(crtc);
	unsigned long flags;

	spin_lock_irqsave(&mcrtc->irq_lock, flags);
	mcrtc->irq_ena &= ~VSYNC_IRQ;
	writel(mcrtc->irq_ena, mcrtc->regs + LCD_SPU_IRQ_ENA);
	spin_unlock_irqrestore(&mcrtc->irq_lock, flags);
}

static irqreturn_t mmp_crtc_irq(int irq, void *data)
{
	struct mmp_crtc *mcrtc = data;
	struct drm_crtc *crtc = &mcrtc->base;
	u32 stat, handled;
	unsigned long flags;

	stat = readl(mcrtc->regs + LCD_SPU_IRQ_ISR);

	spin_lock_irqsave(&mcrtc->irq_lock, flags);
	handled = stat & mcrtc->irq_ena;
	if (handled)
		writel(~handled, mcrtc->regs + LCD_SPU_IRQ_ISR);
	spin_unlock_irqrestore(&mcrtc->irq_lock, flags);

	if (!handled)
		return IRQ_NONE;

	if (handled & VSYNC_IRQ) {
		drm_crtc_handle_vblank(crtc);

		spin_lock_irqsave(&crtc->dev->event_lock, flags);
		if (mcrtc->event) {
			drm_crtc_send_vblank_event(crtc, mcrtc->event);
			mcrtc->event = NULL;
			drm_crtc_vblank_put(crtc);
		}
		spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
	}

	return IRQ_HANDLED;
}

static const struct drm_crtc_helper_funcs mmp_crtc_helper_funcs = {
	.atomic_check = mmp_crtc_atomic_check,
	.mode_set_nofb = mmp_crtc_mode_set_nofb,
	.atomic_enable = mmp_crtc_atomic_enable,
	.atomic_disable = mmp_crtc_atomic_disable,
	.atomic_flush = mmp_crtc_atomic_flush,
};

static const struct drm_crtc_funcs mmp_crtc_funcs = {
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.destroy = drm_crtc_cleanup,
	.reset = drm_atomic_helper_crtc_reset,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
	.gamma_set = drm_atomic_helper_legacy_gamma_set,
	.enable_vblank = mmp_crtc_enable_vblank,
	.disable_vblank = mmp_crtc_disable_vblank,
};

int mmp_crtc_init(struct drm_device *drm, struct mmp_crtc *mcrtc,
		  const struct mmp_lcd_hw *hw)
{
	int ret;

	mcrtc->regs = hw->regs;
	mcrtc->irq = hw->irq;
	mcrtc->clk_hclk = hw->clk_hclk;
	mcrtc->clk_axi = hw->clk_axi;
	mcrtc->clk_lcd = hw->clk_lcd;
	mcrtc->clk_dsi_phy = hw->clk_dsi_phy;
	mcrtc->clk_dsi_esc = hw->clk_dsi_esc;
	mcrtc->takeover = hw->takeover;
	spin_lock_init(&mcrtc->irq_lock);

	ret = mmp_plane_init(drm, &mcrtc->primary, mcrtc->regs, BIT(0), false);
	if (ret)
		return ret;

	ret = mmp_plane_init(drm, &mcrtc->overlay, mcrtc->regs, BIT(0), true);
	if (ret)
		return ret;

	ret = drm_crtc_init_with_planes(drm, &mcrtc->base,
					&mcrtc->primary.base, NULL,
					&mmp_crtc_funcs, "mmp-crtc");
	if (ret)
		return ret;

	drm_crtc_helper_add(&mcrtc->base, &mmp_crtc_helper_funcs);
	drm_crtc_enable_color_mgmt(&mcrtc->base, 0, false, MMP_GAMMA_LUT_SIZE);
	return 0;
}

int mmp_crtc_irq_init(struct mmp_crtc *mcrtc)
{
	int ret;

	ret = mmp_crtc_clks_enable(mcrtc);
	if (ret)
		return ret;

	/* Ensure all LCDC IRQs start masked and cleared cleanly. */
	writel(0, mcrtc->regs + LCD_SPU_IRQ_ENA);
	writel(0, mcrtc->regs + LCD_SPU_IRQ_ISR);

	ret = request_irq(mcrtc->irq, mmp_crtc_irq, 0, "mmp-lcdc", mcrtc);
	if (!ret)
		mcrtc->irq_registered = true;

	if (ret || !mcrtc->takeover)
		mmp_crtc_clks_disable(mcrtc);

	return ret;
}

void mmp_crtc_fini(struct mmp_crtc *mcrtc)
{
	unsigned long flags;
	bool clk_ok;

	if (!mcrtc->irq_registered)
		return;

	clk_ok = !mmp_crtc_clks_enable(mcrtc);
	if (clk_ok) {
		spin_lock_irqsave(&mcrtc->irq_lock, flags);
		mcrtc->irq_ena = 0;
		writel(0, mcrtc->regs + LCD_SPU_IRQ_ENA);
		spin_unlock_irqrestore(&mcrtc->irq_lock, flags);
	}

	free_irq(mcrtc->irq, mcrtc);
	mcrtc->irq_registered = false;
	if (clk_ok)
		mmp_crtc_clks_disable(mcrtc);
}
