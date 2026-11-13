// SPDX-License-Identifier: GPL-2.0-only
/*
 * QuickLogic VX5B3D MIPI DSI to LVDS bridge and PWM backlight driver.
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/regulator/consumer.h>

#include <drm/drm_bridge.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_of.h>
#include <drm/drm_panel.h>

#define VX5B3D_REG_PWM_CTRL		0x0138
#define VX5B3D_REG_PWM_EN		0x015c
#define VX5B3D_REG_PWM_PERIOD		0x0160
#define VX5B3D_REG_PWM_DUTY		0x0164
#define VX5B3D_REG_LVDS_TX_CTRL		0x0604

#define VX5B3D_LVDS_TX_ENABLE		0x3fffffe0
#define VX5B3D_LVDS_TX_DISABLE		0x3fffffff
#define VX5B3D_PWM_MAX_DUTY		2032
#define VX5B3D_PWM_MIN_DUTY		16
#define VX5B3D_DEFAULT_BRIGHTNESS	160
#define VX5B3D_MAX_BRIGHTNESS		255

struct vx5b3d_reg_val {
	u16 reg;
	u32 val;
};

struct vx5b3d {
	struct device *dev;
	struct i2c_client *client;
	struct mipi_dsi_device *dsi;
	struct drm_bridge bridge;
	struct drm_bridge *next_bridge;
	struct backlight_device *bl;

	struct regulator *vdd12;
	struct regulator *vdd18;
	struct gpio_desc *pwr_en_gpio;
	struct gpio_desc *reset_gpio;
	struct gpio_desc *led_en_gpio;

	/* Serializes bridge enable/disable state and I2C/DSI register access. */
	struct mutex lock;
	bool enabled;
	int brightness;
};

static const struct vx5b3d_reg_val vx5b3d_init_pll1[] = {
	{ 0x0700, 0x6c900040 },
	{ 0x0704, 0x0003040a },
	{ 0x070c, 0x00004604 },
	{ 0x0710, 0x054d000b },
	{ 0x0714, 0x00000020 },
	{ 0x0718, 0x00000102 },
	{ 0x071c, 0x00a8002f },
	{ 0x0720, 0x00000000 },
	{ 0x0154, 0x00000000 },
	{ 0x0154, 0x80000000 },
};

static const struct vx5b3d_reg_val vx5b3d_init_pll2[] = {
	{ 0x0700, 0x6c900840 },
	{ 0x070c, 0x00005e56 },
	{ 0x0718, 0x00000202 },
	{ 0x0154, 0x00000000 },
	{ 0x0154, 0x80000000 },
};

static const struct vx5b3d_reg_val vx5b3d_init_video[] = {
	{ 0x037c, 0x00001063 },
	{ 0x0380, 0x82a86030 },
	{ 0x0384, 0x2861408b },
	{ 0x0388, 0x00130285 },
	{ 0x038c, 0x10630009 },
	{ 0x0394, 0x400b82a8 },
	{ 0x0600, 0x016cc78c },
	{ 0x0604, VX5B3D_LVDS_TX_DISABLE },
	{ 0x0608, 0x00000d8c },
	{ 0x0154, 0x00000000 },
	{ 0x0154, 0x80000000 },
	{ 0x0120, 0x00000005 },
	{ 0x0124, 0x1892c400 },
	{ 0x0128, 0x00102806 },
	{ 0x012c, 0x00000062 },
	{ 0x0130, 0x00003c18 },
	{ 0x0134, 0x00000015 },
	{ 0x0138, 0x00ff8000 },
	{ 0x013c, 0x00000000 },
	{ 0x0114, 0x000c6302 },
	{ 0x0140, 0x00010000 },
	{ 0x0174, 0x00000000 },
	{ 0x0404, 0x55550822 },
	{ 0x0418, 0x555502ff },
	{ 0x0410, 0x05e50300 },
	{ 0x020c, 0x00000124 },
	{ 0x021c, 0x00000000 },
	{ 0x0224, 0x00000007 },
	{ 0x0228, 0x00050001 },
	{ 0x022c, 0x0000ff03 },
	{ 0x0230, 0x00000001 },
	{ 0x0234, 0xca033e10 },
	{ 0x0238, 0x00000060 },
	{ 0x023c, 0x82e86030 },
	{ 0x0244, 0x001e0285 },
	{ 0x0258, 0x00060062 },
	{ 0x0400, 0x00000000 },
	{ VX5B3D_REG_PWM_PERIOD, 2040 },
	{ VX5B3D_REG_PWM_DUTY, 0 },
	{ VX5B3D_REG_PWM_EN, 0 },
	{ 0x0158, 0x00000000 },
	{ 0x0158, 0x00000001 },
};

static inline struct vx5b3d *bridge_to_vx5b3d(struct drm_bridge *bridge)
{
	return container_of(bridge, struct vx5b3d, bridge);
}

static int vx5b3d_dsi_write_reg(struct vx5b3d *vx, u16 reg, u32 val)
{
	u8 buf[9] = {
		0x05, 0x01, 0x40,
		reg & 0xff, (reg >> 8) & 0xff,
		val & 0xff, (val >> 8) & 0xff,
		(val >> 16) & 0xff, (val >> 24) & 0xff,
	};
	ssize_t ret;

	ret = mipi_dsi_generic_write(vx->dsi, buf, sizeof(buf));
	return ret < 0 ? (int)ret : 0;
}

static int vx5b3d_dsi_write_seq(struct vx5b3d *vx,
				const struct vx5b3d_reg_val *seq, size_t n)
{
	size_t i;
	int ret;

	for (i = 0; i < n; i++) {
		ret = vx5b3d_dsi_write_reg(vx, seq[i].reg, seq[i].val);
		if (ret)
			return ret;
	}

	return 0;
}

static int vx5b3d_i2c_write_reg(struct vx5b3d *vx, u16 reg, u32 val)
{
	u8 buf[11] = {
		0x09, 0x29, 0x05, 0x01, 0x40,
		reg & 0xff, (reg >> 8) & 0xff,
		val & 0xff, (val >> 8) & 0xff,
		(val >> 16) & 0xff, (val >> 24) & 0xff,
	};
	int ret;

	ret = i2c_master_send(vx->client, buf, sizeof(buf));
	return ret < 0 ? ret : (ret == sizeof(buf) ? 0 : -EIO);
}

static int __maybe_unused vx5b3d_i2c_read_reg(struct vx5b3d *vx, u16 reg,
					      u32 *val)
{
	struct i2c_adapter *adap = vx->client->adapter;
	u8 tx_addr[9] = {
		0x09, 0x29, 0x05, 0x01, 0x41,
		reg & 0xff, (reg >> 8) & 0xff, 0x04, 0x00,
	};
	u8 tx_cmd[4] = { 0x09, 0x24, 0x05, 0x01 };
	u8 rx_data[4];
	struct i2c_msg msgs[3] = {
		{
			.addr = vx->client->addr,
			.flags = 0,
			.len = sizeof(tx_addr),
			.buf = tx_addr,
		},
		{
			.addr = vx->client->addr,
			.flags = 0,
			.len = sizeof(tx_cmd),
			.buf = tx_cmd,
		},
		{
			.addr = vx->client->addr,
			.flags = I2C_M_RD,
			.len = sizeof(rx_data),
			.buf = rx_data,
		},
	};
	int ret, i;

	/*
	 * VX5B3D CSR readback requires three separate I2C transactions with STOP
	 * boundaries held atomically across bus lock so shared touch traffic
	 * on twsi1 cannot interleave between address latch and read.
	 */
	i2c_lock_bus(adap, I2C_LOCK_SEGMENT);
	for (i = 0; i < ARRAY_SIZE(msgs); i++) {
		ret = __i2c_transfer(adap, &msgs[i], 1);
		if (ret < 0)
			goto out_unlock;
		if (ret != 1) {
			ret = -EIO;
			goto out_unlock;
		}
	}
	*val = rx_data[0] | ((u32)rx_data[1] << 8) |
	       ((u32)rx_data[2] << 16) | ((u32)rx_data[3] << 24);
	ret = 0;

out_unlock:
	i2c_unlock_bus(adap, I2C_LOCK_SEGMENT);
	return ret;
}

static u32 vx5b3d_brightness_to_duty(int brightness)
{
	u32 duty;

	if (brightness <= 0)
		return 0;

	duty = DIV_ROUND_CLOSEST(brightness * VX5B3D_PWM_MAX_DUTY,
				 VX5B3D_MAX_BRIGHTNESS);
	return clamp_t(u32, duty, VX5B3D_PWM_MIN_DUTY, VX5B3D_PWM_MAX_DUTY);
}

static int vx5b3d_apply_backlight(struct vx5b3d *vx, int brightness)
{
	u32 duty = vx5b3d_brightness_to_duty(brightness);
	int ret;

	if (duty) {
		gpiod_set_value_cansleep(vx->led_en_gpio, 1);
		ret = vx5b3d_i2c_write_reg(vx, VX5B3D_REG_PWM_CTRL, 0x3fff0000);
		if (ret)
			return ret;
		ret = vx5b3d_i2c_write_reg(vx, VX5B3D_REG_PWM_DUTY, duty);
		if (ret)
			return ret;
		ret = vx5b3d_i2c_write_reg(vx, VX5B3D_REG_PWM_EN, 5);
		if (ret)
			return ret;
	} else {
		ret = vx5b3d_i2c_write_reg(vx, VX5B3D_REG_PWM_DUTY, 0);
		if (ret)
			return ret;
		ret = vx5b3d_i2c_write_reg(vx, VX5B3D_REG_PWM_EN, 0);
		if (ret)
			return ret;
		gpiod_set_value_cansleep(vx->led_en_gpio, 0);
	}

	return 0;
}

static int vx5b3d_bl_update_status(struct backlight_device *bl)
{
	struct vx5b3d *vx = bl_get_data(bl);
	int brightness = bl->props.brightness;
	int ret = 0;

	if (bl->props.power != FB_BLANK_UNBLANK ||
	    bl->props.fb_blank != FB_BLANK_UNBLANK ||
	    (bl->props.state & (BL_CORE_SUSPENDED | BL_CORE_FBBLANK)))
		brightness = 0;

	mutex_lock(&vx->lock);
	vx->brightness = bl->props.brightness;
	if (vx->enabled)
		ret = vx5b3d_apply_backlight(vx, brightness);
	mutex_unlock(&vx->lock);

	return ret;
}

static const struct backlight_ops vx5b3d_bl_ops = {
	.update_status = vx5b3d_bl_update_status,
};

static int vx5b3d_bridge_attach(struct drm_bridge *bridge)
{
	struct vx5b3d *vx = bridge_to_vx5b3d(bridge);

	return drm_bridge_attach(bridge->encoder, vx->next_bridge, bridge);
}

static void vx5b3d_bridge_pre_enable(struct drm_bridge *bridge)
{
	struct vx5b3d *vx = bridge_to_vx5b3d(bridge);
	int ret;

	mutex_lock(&vx->lock);

	gpiod_set_value_cansleep(vx->pwr_en_gpio, 1);
	usleep_range(10000, 12000);
	gpiod_set_value_cansleep(vx->reset_gpio, 1);
	usleep_range(1000, 2000);
	gpiod_set_value_cansleep(vx->reset_gpio, 0);
	msleep(32);

	ret = vx5b3d_dsi_write_seq(vx, vx5b3d_init_pll1,
				   ARRAY_SIZE(vx5b3d_init_pll1));
	if (ret)
		goto err_dsi;
	usleep_range(1500, 2500);

	ret = vx5b3d_dsi_write_seq(vx, vx5b3d_init_pll2,
				   ARRAY_SIZE(vx5b3d_init_pll2));
	if (ret)
		goto err_dsi;
	usleep_range(1500, 2500);

	ret = vx5b3d_dsi_write_seq(vx, vx5b3d_init_video,
				   ARRAY_SIZE(vx5b3d_init_video));
	if (ret)
		goto err_dsi;
	usleep_range(10000, 12000);

	mutex_unlock(&vx->lock);
	return;

err_dsi:
	dev_err(vx->dev, "DSI initialization failed: %d\n", ret);
	mutex_unlock(&vx->lock);
}

static void vx5b3d_bridge_enable(struct drm_bridge *bridge)
{
	struct vx5b3d *vx = bridge_to_vx5b3d(bridge);
	int attempt, ret = 0;

	mutex_lock(&vx->lock);

	/*
	 * DSI 4-lane video stream and LCDC DMA are already live here.
	 * Allow DSI HS clock/link a brief settle window, ungate LVDS TX,
	 * wait 110 ms for panel TCON lock, then turn on PWM + RT8511B boost
	 * to avoid white flash at power-on/unblank.
	 */
	usleep_range(2000, 3000);
	for (attempt = 0; attempt < 10; attempt++) {
		ret = vx5b3d_i2c_write_reg(vx, VX5B3D_REG_LVDS_TX_CTRL,
					   VX5B3D_LVDS_TX_ENABLE);
		if (!ret)
			break;
		usleep_range(5000, 6000);
	}
	if (ret) {
		dev_err(vx->dev, "failed to enable LVDS TX: %d\n", ret);
		goto out_unlock;
	}
	if (attempt)
		dev_info(vx->dev, "enabled LVDS TX after %d I2C retries\n",
			 attempt);

	msleep(110);

	vx->enabled = true;
	ret = vx5b3d_apply_backlight(vx, vx->brightness);
	if (ret)
		dev_err(vx->dev, "failed to enable backlight: %d\n", ret);

out_unlock:
	mutex_unlock(&vx->lock);
}

static void vx5b3d_bridge_disable(struct drm_bridge *bridge)
{
	struct vx5b3d *vx = bridge_to_vx5b3d(bridge);

	mutex_lock(&vx->lock);
	vx->enabled = false;
	vx5b3d_apply_backlight(vx, 0);
	msleep(100);
	vx5b3d_i2c_write_reg(vx, VX5B3D_REG_LVDS_TX_CTRL,
			     VX5B3D_LVDS_TX_DISABLE);
	msleep(30);
	mutex_unlock(&vx->lock);
}

static void vx5b3d_bridge_post_disable(struct drm_bridge *bridge)
{
	struct vx5b3d *vx = bridge_to_vx5b3d(bridge);

	/*
	 * Keep vdd12 and vdd18 (LDO9) powered across runtime DPMS off because
	 * twsi1 I2C IO rail is shared with Zinitix BT532 touch on this board.
	 */
	mutex_lock(&vx->lock);
	gpiod_set_value_cansleep(vx->reset_gpio, 1);
	gpiod_set_value_cansleep(vx->pwr_en_gpio, 0);
	mutex_unlock(&vx->lock);
}

static const struct drm_bridge_funcs vx5b3d_bridge_funcs = {
	.attach = vx5b3d_bridge_attach,
	.pre_enable = vx5b3d_bridge_pre_enable,
	.enable = vx5b3d_bridge_enable,
	.disable = vx5b3d_bridge_disable,
	.post_disable = vx5b3d_bridge_post_disable,
};

static void vx5b3d_disable_regulators(void *data)
{
	struct vx5b3d *vx = data;

	regulator_disable(vx->vdd18);
	regulator_disable(vx->vdd12);
}

static int vx5b3d_attach_dsi(struct vx5b3d *vx)
{
	struct mipi_dsi_device_info info = {
		.type = "vx5b3d",
		.channel = 0,
		.node = NULL,
	};
	struct device_node *ep, *host_node;
	struct mipi_dsi_host *host;
	struct mipi_dsi_device *dsi;
	int ret;

	ep = of_graph_get_endpoint_by_regs(vx->dev->of_node, 0, -1);
	if (!ep)
		return -ENODEV;

	host_node = of_graph_get_remote_port_parent(ep);
	of_node_put(ep);
	if (!host_node)
		return -ENODEV;

	host = of_find_mipi_dsi_host_by_node(host_node);
	of_node_put(host_node);
	if (!host)
		return -EPROBE_DEFER;

	dsi = mipi_dsi_device_register_full(host, &info);
	if (IS_ERR(dsi))
		return PTR_ERR(dsi);

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_SYNC_PULSE |
			  MIPI_DSI_MODE_LPM;
	vx->dsi = dsi;

	ret = mipi_dsi_attach(dsi);
	if (ret) {
		vx->dsi = NULL;
		mipi_dsi_device_unregister(dsi);
		return ret;
	}

	return 0;
}

static int vx5b3d_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct backlight_properties bl_props = {};
	struct drm_panel *panel = NULL;
	struct drm_bridge *next_bridge = NULL;
	struct vx5b3d *vx;
	int ret;

	ret = drm_of_find_panel_or_bridge(dev->of_node, 1, 0,
					  &panel, &next_bridge);
	if (ret)
		return ret;

	if (panel) {
		next_bridge = devm_drm_panel_bridge_add(dev, panel,
							DRM_MODE_CONNECTOR_LVDS);
		if (IS_ERR(next_bridge))
			return PTR_ERR(next_bridge);
	}

	vx = devm_kzalloc(dev, sizeof(*vx), GFP_KERNEL);
	if (!vx)
		return -ENOMEM;

	vx->dev = dev;
	vx->client = client;
	vx->next_bridge = next_bridge;
	vx->brightness = VX5B3D_DEFAULT_BRIGHTNESS;
	mutex_init(&vx->lock);
	i2c_set_clientdata(client, vx);

	vx->vdd12 = devm_regulator_get(dev, "vdd12");
	if (IS_ERR(vx->vdd12))
		return PTR_ERR(vx->vdd12);

	vx->vdd18 = devm_regulator_get(dev, "vdd18");
	if (IS_ERR(vx->vdd18))
		return PTR_ERR(vx->vdd18);

	vx->pwr_en_gpio = devm_gpiod_get_optional(dev, "enable", GPIOD_OUT_HIGH);
	if (IS_ERR(vx->pwr_en_gpio))
		return PTR_ERR(vx->pwr_en_gpio);

	vx->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(vx->reset_gpio))
		return PTR_ERR(vx->reset_gpio);

	vx->led_en_gpio = devm_gpiod_get_optional(dev, "backlight-enable",
						  GPIOD_OUT_HIGH);
	if (IS_ERR(vx->led_en_gpio))
		return PTR_ERR(vx->led_en_gpio);

	ret = regulator_enable(vx->vdd12);
	if (ret)
		return ret;

	ret = regulator_enable(vx->vdd18);
	if (ret) {
		regulator_disable(vx->vdd12);
		return ret;
	}

	ret = devm_add_action_or_reset(dev, vx5b3d_disable_regulators, vx);
	if (ret)
		return ret;

	bl_props.type = BACKLIGHT_RAW;
	bl_props.max_brightness = VX5B3D_MAX_BRIGHTNESS;
	bl_props.brightness = vx->brightness;
	vx->bl = devm_backlight_device_register(dev, "vx5b3d-backlight", dev,
						vx, &vx5b3d_bl_ops, &bl_props);
	if (IS_ERR(vx->bl))
		return PTR_ERR(vx->bl);

	vx->bridge.funcs = &vx5b3d_bridge_funcs;
	vx->bridge.of_node = dev->of_node;
	drm_bridge_add(&vx->bridge);

	ret = vx5b3d_attach_dsi(vx);
	if (ret) {
		drm_bridge_remove(&vx->bridge);
		return ret;
	}

	return 0;
}

static int vx5b3d_remove(struct i2c_client *client)
{
	struct vx5b3d *vx = i2c_get_clientdata(client);

	mipi_dsi_detach(vx->dsi);
	mipi_dsi_device_unregister(vx->dsi);
	drm_bridge_remove(&vx->bridge);
	return 0;
}

static const struct i2c_device_id vx5b3d_i2c_id[] = {
	{ "vx5b3d", 0 },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(i2c, vx5b3d_i2c_id);

static const struct of_device_id vx5b3d_of_match[] = {
	{ .compatible = "quicklogic,vx5b3d", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, vx5b3d_of_match);

static struct i2c_driver vx5b3d_i2c_driver = {
	.driver = {
		.name = "quicklogic-vx5b3d",
		.of_match_table = vx5b3d_of_match,
	},
	.probe = vx5b3d_probe,
	.remove = vx5b3d_remove,
	.id_table = vx5b3d_i2c_id,
};

module_i2c_driver(vx5b3d_i2c_driver);

MODULE_AUTHOR("goyawifi 4.19 porting project");
MODULE_DESCRIPTION("QuickLogic VX5B3D MIPI-to-LVDS Bridge and Backlight Driver");
MODULE_LICENSE("GPL v2");
