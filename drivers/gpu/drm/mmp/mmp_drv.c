// SPDX-License-Identifier: GPL-2.0-only
/*
 * Marvell PXA988/MMP atomic DRM/KMS display driver.
 */

#include <linux/component.h>
#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/of_graph.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_gem_cma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_modeset_helper.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_of.h>
#include <drm/drm_vblank.h>

#include "mmp_drv.h"

#define MMP_MAX_WIDTH	2048
#define MMP_MAX_HEIGHT	2048

static const struct drm_mode_config_funcs mmp_mode_config_funcs = {
	.fb_create = drm_gem_fb_create,
	.output_poll_changed = drm_fb_helper_output_poll_changed,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static const struct drm_mode_config_helper_funcs mmp_mode_config_helper_funcs = {
	.atomic_commit_tail = drm_atomic_helper_commit_tail_rpm,
};

static int mmp_takeover_get_modes(struct drm_connector *connector)
{
	struct drm_display_mode *mode;

	mode = drm_mode_create(connector->dev);
	if (!mode)
		return 0;

	mode->clock = 51919;
	mode->hdisplay = 1024;
	mode->hsync_start = 1147;
	mode->hsync_end = 1267;
	mode->htotal = 1367;
	mode->vdisplay = 600;
	mode->vsync_start = 613;
	mode->vsync_end = 623;
	mode->vtotal = 633;
	mode->flags = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC;
	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_set_name(mode);
	drm_mode_probed_add(connector, mode);

	connector->display_info.width_mm = 154;
	connector->display_info.height_mm = 90;

	return 1;
}

static const struct drm_connector_helper_funcs mmp_takeover_conn_helper_funcs = {
	.get_modes = mmp_takeover_get_modes,
};

static const struct drm_connector_funcs mmp_takeover_conn_funcs = {
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static const struct drm_encoder_funcs mmp_takeover_enc_funcs = {
	.destroy = drm_encoder_cleanup,
};

static int mmp_create_takeover_output(struct mmp_drm_private *priv)
{
	struct drm_device *drm = &priv->drm;
	struct drm_encoder *encoder = &priv->encoder;
	struct drm_connector *connector = &priv->connector;
	int ret;

	encoder->possible_crtcs = BIT(0);
	ret = drm_encoder_init(drm, encoder, &mmp_takeover_enc_funcs,
			       DRM_MODE_ENCODER_DSI, NULL);
	if (ret)
		return ret;

	ret = drm_connector_init(drm, connector, &mmp_takeover_conn_funcs,
				 DRM_MODE_CONNECTOR_DSI);
	if (ret)
		return ret;

	drm_connector_helper_add(connector, &mmp_takeover_conn_helper_funcs);
	connector->status = connector_status_connected;

	return drm_connector_attach_encoder(connector, encoder);
}

DEFINE_DRM_GEM_CMA_FOPS(mmp_drm_fops);

static void mmp_drm_release(struct drm_device *drm)
{
	struct mmp_drm_private *priv = to_mmp_drm(drm);

	drm_mode_config_cleanup(drm);
	drm_dev_fini(drm);
	kfree(priv);
}

static struct drm_driver mmp_drm_driver = {
	.driver_features = DRIVER_MODESET | DRIVER_GEM | DRIVER_PRIME |
			   DRIVER_ATOMIC,
	.release = mmp_drm_release,
	.lastclose = drm_fb_helper_lastclose,
	.fops = &mmp_drm_fops,
	.dumb_create = drm_gem_cma_dumb_create,
	.prime_handle_to_fd = drm_gem_prime_handle_to_fd,
	.prime_fd_to_handle = drm_gem_prime_fd_to_handle,
	.gem_free_object_unlocked = drm_gem_cma_free_object,
	.gem_vm_ops = &drm_gem_cma_vm_ops,
	.gem_prime_export = drm_gem_prime_export,
	.gem_prime_import = drm_gem_prime_import,
	.gem_prime_get_sg_table = drm_gem_cma_prime_get_sg_table,
	.gem_prime_import_sg_table = drm_gem_cma_prime_import_sg_table,
	.gem_prime_vmap = drm_gem_cma_prime_vmap,
	.gem_prime_vunmap = drm_gem_cma_prime_vunmap,
	.gem_prime_mmap = drm_gem_cma_prime_mmap,
	.name = "mmp-drm",
	.desc = "Marvell PXA988/MMP Display Controller",
	.date = "20260401",
	.major = 1,
	.minor = 0,
};

static struct clk *mmp_get_optional_clk(struct device *dev, const char *name)
{
	struct clk *clk = devm_clk_get(dev, name);

	if (IS_ERR(clk) && PTR_ERR(clk) == -ENOENT)
		return NULL;

	return clk;
}

static int mmp_drm_bind(struct device *dev)
{
	struct mmp_lcd_hw *hw = dev_get_drvdata(dev);
	struct mmp_drm_private *priv;
	struct drm_device *drm;
	int ret;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	drm = &priv->drm;
	ret = drm_dev_init(drm, &mmp_drm_driver, dev);
	if (ret) {
		kfree(priv);
		return ret;
	}

	drm_mode_config_init(drm);
	drm->mode_config.min_width = 16;
	drm->mode_config.min_height = 16;
	drm->mode_config.max_width = MMP_MAX_WIDTH;
	drm->mode_config.max_height = MMP_MAX_HEIGHT;
	drm->mode_config.funcs = &mmp_mode_config_funcs;
	drm->mode_config.helper_private = &mmp_mode_config_helper_funcs;

	ret = mmp_crtc_init(drm, &priv->crtc, hw);
	if (ret)
		goto err_drm_put;

	if (priv->crtc.takeover) {
		ret = mmp_create_takeover_output(priv);
		if (ret)
			goto err_crtc_fini;
	} else {
		ret = component_bind_all(dev, drm);
		if (ret)
			goto err_crtc_fini;
	}

	ret = mmp_crtc_irq_init(&priv->crtc);
	if (ret)
		goto err_unbind;

	ret = drm_vblank_init(drm, drm->mode_config.num_crtc);
	if (ret)
		goto err_unbind;

	drm_mode_config_reset(drm);
	drm_kms_helper_poll_init(drm);

	ret = drm_fb_helper_remove_conflicting_framebuffers(NULL, "mmp-drm",
							    false);
	if (ret)
		goto err_poll_fini;

	ret = drm_dev_register(drm, 0);
	if (ret)
		goto err_poll_fini;

	hw->priv = priv;
	drm_fbdev_generic_setup(drm, 32);
	return 0;

err_poll_fini:
	drm_kms_helper_poll_fini(drm);
err_unbind:
	if (!priv->crtc.takeover)
		component_unbind_all(dev, drm);
err_crtc_fini:
	mmp_crtc_fini(&priv->crtc);
err_drm_put:
	drm_dev_put(drm);
	return ret;
}

static void mmp_drm_unbind(struct device *dev)
{
	struct mmp_lcd_hw *hw = dev_get_drvdata(dev);
	struct mmp_drm_private *priv = hw->priv;
	struct drm_device *drm = &priv->drm;

	hw->priv = NULL;
	drm_dev_unregister(drm);
	drm_kms_helper_poll_fini(drm);
	if (!priv->crtc.takeover)
		component_unbind_all(dev, drm);
	mmp_crtc_fini(&priv->crtc);
	drm_dev_put(drm);
}

static const struct component_master_ops mmp_drm_master_ops = {
	.bind = mmp_drm_bind,
	.unbind = mmp_drm_unbind,
};

static int compare_of(struct device *dev, void *data)
{
	return dev->of_node == data;
}

static int mmp_drm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct component_match *match = NULL;
	struct device_node *ep, *remote;
	struct mmp_lcd_hw *hw;
	struct resource *res;
	int ret;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	hw = devm_kzalloc(dev, sizeof(*hw), GFP_KERNEL);
	if (!hw)
		return -ENOMEM;

	hw->dev = dev;
	hw->takeover = of_property_read_bool(dev->of_node,
					     "marvell,bootloader-takeover");

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	hw->regs = devm_ioremap_resource(dev, res);
	if (IS_ERR(hw->regs))
		return PTR_ERR(hw->regs);

	hw->irq = platform_get_irq(pdev, 0);
	if (hw->irq < 0)
		return hw->irq;

	hw->clk_hclk = devm_clk_get(dev, "hclk");
	if (IS_ERR(hw->clk_hclk))
		return PTR_ERR(hw->clk_hclk);

	hw->clk_axi = devm_clk_get(dev, "axi");
	if (IS_ERR(hw->clk_axi))
		return PTR_ERR(hw->clk_axi);

	hw->clk_lcd = devm_clk_get(dev, "lcd");
	if (IS_ERR(hw->clk_lcd))
		return PTR_ERR(hw->clk_lcd);

	hw->clk_dsi_phy = mmp_get_optional_clk(dev, "dsi_phy");
	if (IS_ERR(hw->clk_dsi_phy))
		return PTR_ERR(hw->clk_dsi_phy);

	hw->clk_dsi_esc = mmp_get_optional_clk(dev, "dsi_esc");
	if (IS_ERR(hw->clk_dsi_esc))
		return PTR_ERR(hw->clk_dsi_esc);

	platform_set_drvdata(pdev, hw);

	if (hw->takeover)
		return mmp_drm_bind(dev);

	for_each_endpoint_of_node(dev->of_node, ep) {
		remote = of_graph_get_remote_port_parent(ep);
		if (!remote)
			continue;
		if (of_device_is_available(remote))
			drm_of_component_match_add(dev, &match,
						   compare_of, remote);
		of_node_put(remote);
	}

	if (!match)
		return -ENODEV;

	return component_master_add_with_match(dev, &mmp_drm_master_ops,
					       match);
}

static int mmp_drm_remove(struct platform_device *pdev)
{
	struct mmp_lcd_hw *hw = platform_get_drvdata(pdev);

	if (hw->takeover)
		mmp_drm_unbind(&pdev->dev);
	else
		component_master_del(&pdev->dev, &mmp_drm_master_ops);

	return 0;
}

static const struct of_device_id mmp_drm_dt_ids[] = {
	{ .compatible = "marvell,pxa988-lcd", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mmp_drm_dt_ids);

static int __maybe_unused mmp_drm_suspend(struct device *dev)
{
	struct mmp_lcd_hw *hw = dev_get_drvdata(dev);

	return (hw && hw->priv) ?
	       drm_mode_config_helper_suspend(&hw->priv->drm) : 0;
}

static int __maybe_unused mmp_drm_resume(struct device *dev)
{
	struct mmp_lcd_hw *hw = dev_get_drvdata(dev);

	return (hw && hw->priv) ?
	       drm_mode_config_helper_resume(&hw->priv->drm) : 0;
}

static SIMPLE_DEV_PM_OPS(mmp_drm_pm_ops, mmp_drm_suspend, mmp_drm_resume);

static struct platform_driver mmp_drm_platform_driver = {
	.probe = mmp_drm_probe,
	.remove = mmp_drm_remove,
	.driver = {
		.name = "mmp-drm",
		.of_match_table = mmp_drm_dt_ids,
		.pm = &mmp_drm_pm_ops,
	},
};

static struct platform_driver *const mmp_drivers[] = {
	&mmp_drm_platform_driver,
	&mmp_dsi_driver,
};

static int __init mmp_drm_init(void)
{
	return platform_register_drivers(mmp_drivers, ARRAY_SIZE(mmp_drivers));
}

static void __exit mmp_drm_exit(void)
{
	platform_unregister_drivers(mmp_drivers, ARRAY_SIZE(mmp_drivers));
}

module_init(mmp_drm_init);
module_exit(mmp_drm_exit);

MODULE_AUTHOR("goyawifi 4.19 porting project");
MODULE_DESCRIPTION("Marvell PXA988/MMP Atomic DRM/KMS Driver");
MODULE_LICENSE("GPL v2");
