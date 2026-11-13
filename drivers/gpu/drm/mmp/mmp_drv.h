/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MMP_DRV_H__
#define __MMP_DRV_H__

#include <linux/clk.h>
#include <linux/spinlock.h>
#include <drm/drm_crtc.h>
#include <drm/drm_device.h>
#include <drm/drm_encoder.h>
#include <drm/drm_plane.h>

struct mmp_plane {
	struct drm_plane base;
	void __iomem *regs;
	bool is_overlay;
};

struct mmp_lcd_hw {
	struct device *dev;
	void __iomem *regs;
	int irq;

	struct clk *clk_hclk;
	struct clk *clk_axi;
	struct clk *clk_lcd;
	struct clk *clk_dsi_phy;
	struct clk *clk_dsi_esc;
	bool takeover;

	struct mmp_drm_private *priv;
};

struct mmp_crtc {
	struct drm_crtc base;
	struct mmp_plane primary;
	struct mmp_plane overlay;
	void __iomem *regs;
	int irq;

	struct clk *clk_hclk;
	struct clk *clk_axi;
	struct clk *clk_lcd;
	struct clk *clk_dsi_phy;
	struct clk *clk_dsi_esc;

	/* Serializes IRQ enable/ACK updates and irq_ena state. */
	spinlock_t irq_lock;
	u32 irq_ena;
	struct drm_pending_vblank_event *event;
	bool takeover;
	bool clks_on;
	bool irq_registered;
};

struct mmp_drm_private {
	struct drm_device drm;
	struct mmp_crtc crtc;
	struct drm_encoder encoder;
	struct drm_connector connector;
};

static inline struct mmp_crtc *to_mmp_crtc(struct drm_crtc *crtc)
{
	return container_of(crtc, struct mmp_crtc, base);
}

static inline struct mmp_plane *to_mmp_plane(struct drm_plane *plane)
{
	return container_of(plane, struct mmp_plane, base);
}

static inline struct mmp_drm_private *to_mmp_drm(struct drm_device *drm)
{
	return container_of(drm, struct mmp_drm_private, drm);
}

struct platform_driver;

int mmp_plane_init(struct drm_device *drm, struct mmp_plane *plane,
		   void __iomem *regs, unsigned int possible_crtcs,
		   bool is_overlay);
int mmp_crtc_init(struct drm_device *drm, struct mmp_crtc *crtc,
		  const struct mmp_lcd_hw *hw);
int mmp_crtc_irq_init(struct mmp_crtc *crtc);
void mmp_crtc_fini(struct mmp_crtc *crtc);

extern struct platform_driver mmp_dsi_driver;

#endif /* __MMP_DRV_H__ */
