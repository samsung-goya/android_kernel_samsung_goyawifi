// SPDX-License-Identifier: GPL-2.0-only
/*
 * Marvell PXA988/MMP MIPI DSI host controller and D-PHY driver.
 */

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/component.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mutex.h>
#include <linux/of_graph.h>
#include <linux/platform_device.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_bridge.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_encoder.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_of.h>
#include <drm/drm_panel.h>
#include <video/mipi_display.h>

#include "mmp_drv.h"
#include "mmp_regs.h"

#define MMP_DSI_MAX_TX_BYTES	255
#define MMP_DSI_DEFAULT_BITCLK_KHZ	312000

struct mmp_dsi {
	struct device *dev;
	struct mipi_dsi_host host;
	struct drm_encoder encoder;
	struct drm_bridge *bridge;
	struct drm_bridge *panel_bridge;
	struct drm_display_mode mode;

	void __iomem *regs;
	struct clk *clk_dsi_phy;
	struct clk *clk_dsi_esc;

	/* Serializes DSI FIFO access, transfers, and PHY power transitions. */
	struct mutex tx_mutex;
	u32 lanes;
	u32 active_lanes;
	enum mipi_dsi_pixel_format format;
	bool lp_ready;
	bool video_enabled;
};

static inline struct mmp_dsi *host_to_mmp_dsi(struct mipi_dsi_host *host)
{
	return container_of(host, struct mmp_dsi, host);
}

static inline struct mmp_dsi *enc_to_mmp_dsi(struct drm_encoder *enc)
{
	return container_of(enc, struct mmp_dsi, encoder);
}

/*
 * Linux 4.19 mipi_dsi_create_packet() omits both header ECC and payload CRC16.
 * PXA988 DSI hardware requires software-computed ECC (header[3]) and CCITT
 * CRC16 (0x8408 reflected polynomial, 0xffff initial seed) in CPU command mode.
 */
static u8 mmp_dsi_ecc(const u8 hdr[3])
{
	static const u32 ecc_masks[6] = {
		0xf12cb7, /* P0 */
		0xf2555b, /* P1 */
		0x749a6d, /* P2 */
		0xb8e38e, /* P3 */
		0xdf03f0, /* P4 */
		0xeffc00, /* P5 */
	};
	u32 val = hdr[0] | ((u32)hdr[1] << 8) | ((u32)hdr[2] << 16);
	u8 ecc = 0;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(ecc_masks); i++) {
		if (hweight32(val & ecc_masks[i]) & 1)
			ecc |= BIT(i);
	}

	return ecc;
}

static u16 mmp_dsi_crc16(const u8 *data, size_t len)
{
	u16 crc = 0xffff;
	size_t i;

	for (i = 0; i < len; i++) {
		u8 byte = data[i];
		int bit;

		for (bit = 0; bit < 8; bit++) {
			bool xor_poly = ((crc ^ byte) & 1) != 0;

			crc >>= 1;
			if (xor_poly)
				crc ^= 0x8408;
			byte >>= 1;
		}
	}

	return crc;
}

static void mmp_dsi_set_dphy_timing(struct mmp_dsi *mdsi)
{
	unsigned long esc_rate = clk_get_rate(mdsi->clk_dsi_esc);
	u32 esc_mhz = esc_rate ? DIV_ROUND_CLOSEST(esc_rate, 1000000) : 52;
	u32 esc_t = max(1U, 1000U / esc_mhz);
	u32 ui = 1000U / (MMP_DSI_DEFAULT_BITCLK_KHZ / 1000U) + 1;
	u32 lpx_clk, lpx_time, ta_get, ta_go, wakeup;
	u32 hs_prep, hs_zero, hs_trail, hs_exit, ck_zero, ck_trail;
	u32 rcomp;
	int tmp;

	lpx_clk = max(1U, 50U / esc_t);
	lpx_time = (lpx_clk + 1) * esc_t;
	ta_get = (lpx_time * 5) / esc_t - 1;
	ta_go = (lpx_time * 4) / esc_t - 1;
	wakeup = 1000000U / esc_t + 1;

	hs_prep = (40U + 4U * ui) / esc_t + 1;
	tmp = (int)(175U + 10U * ui) - (int)((hs_prep + 1) * esc_t) -
	      (int)(24U * ui);
	hs_zero = tmp > 0 ? (u32)(tmp / (int)esc_t + 4) : 4U;

	tmp = (int)((160U + 4U * ui) / esc_t + 1) - 3;
	hs_trail = tmp > 0 ? (u32)tmp : 0U;

	hs_exit = 100U / esc_t + 1;
	ck_zero = (300U - (hs_prep + 1) * esc_t) / esc_t + 1;
	ck_trail = 60U / esc_t + 1;

	rcomp = readl(mdsi->regs + DSI_PHY_RCOMP_0);
	rcomp |= BIT(9);
	writel(rcomp, mdsi->regs + DSI_PHY_RCOMP_0);

	writel((hs_exit << 24) | (hs_trail << 16) | (hs_zero << 8) | hs_prep,
	       mdsi->regs + DSI_PHY_TIME_0);
	writel((ta_get << 24) | (ta_go << 16) | (wakeup & 0xffff),
	       mdsi->regs + DSI_PHY_TIME_1);
	writel((hs_exit << 24) | (ck_trail << 16) | (ck_zero << 8) | lpx_clk,
	       mdsi->regs + DSI_PHY_TIME_2);
	writel((lpx_clk << 8) | 0x3c, mdsi->regs + DSI_PHY_TIME_3);
}

static void mmp_dsi_set_video_timings(struct mmp_dsi *mdsi)
{
	const struct drm_display_mode *mode = &mdsi->mode;
	u32 lanes = mdsi->lanes ? mdsi->lanes : 4;
	u32 bpp = 3; /* RGB888 */
	u32 hact, hfp, hsync, hbp, vact, vfp, vsync, vbp, vtot;
	u32 hact_b, hfp_b, hsync_b, hbp_b, htot_b;
	u32 hbp_wc, hfp_wc, hact_wc, htot_blank_wc;

	if (!mode->hdisplay || !mode->vdisplay)
		return;

	hact = mode->hdisplay;
	hfp = mode->hsync_start - mode->hdisplay;
	hsync = mode->hsync_end - mode->hsync_start;
	hbp = mode->htotal - mode->hsync_end;

	vact = mode->vdisplay;
	vfp = mode->vsync_start - mode->vdisplay;
	vsync = mode->vsync_end - mode->vsync_start;
	vbp = mode->vtotal - mode->vsync_end;
	vtot = mode->vtotal;

	hact_b = (hact * bpp) / lanes;
	hfp_b = (hfp * bpp) / lanes;
	hsync_b = (hsync * bpp) / lanes;
	hbp_b = (hbp * bpp) / lanes;
	htot_b = hact_b + hfp_b + hsync_b + hbp_b;

	hbp_wc = hsync * bpp + hbp * bpp > 10 ?
		 (hsync * bpp + hbp * bpp - 10) : 0;
	hfp_wc = hfp * bpp > 12 ? (hfp * bpp - 12) : 0;
	hact_wc = hact * bpp;
	htot_blank_wc = mode->htotal * bpp > 10 ?
			(mode->htotal * bpp - 10) : 0;

	writel(0x00402010, mdsi->regs + DSI_LCD1_CTRL_0);
	writel(DSI_LCD1_VSYNC_RST_EN | DSI_LCD1_HBP_PKT_EN |
	       DSI_LCD1_BURST_SYNC_EVENT | DSI_LCD1_RGB888,
	       mdsi->regs + DSI_LCD1_CTRL_1);

	writel((hact_b << 16) | htot_b, mdsi->regs + DSI_LCD1_TIMING_0);
	writel((hsync_b << 16) | hbp_b, mdsi->regs + DSI_LCD1_TIMING_1);
	writel((vact << 16) | vtot, mdsi->regs + DSI_LCD1_TIMING_2);
	writel((vsync << 16) | vbp, mdsi->regs + DSI_LCD1_TIMING_3);

	writel(hbp_wc << 16, mdsi->regs + DSI_LCD1_WC_0);
	writel((hfp_wc << 16) | hact_wc, mdsi->regs + DSI_LCD1_WC_1);
	writel(htot_blank_wc, mdsi->regs + DSI_LCD1_WC_2);

	(void)vfp;
}

static int mmp_dsi_ensure_lp(struct mmp_dsi *mdsi)
{
	int ret;

	if (mdsi->lp_ready)
		return 0;

	ret = clk_prepare_enable(mdsi->clk_dsi_esc);
	if (ret)
		return ret;

	ret = clk_prepare_enable(mdsi->clk_dsi_phy);
	if (ret) {
		clk_disable_unprepare(mdsi->clk_dsi_esc);
		return ret;
	}

	/* Reset DSI controller cleanly and enter 1-lane LP command mode. */
	writel(0, mdsi->regs + DSI_CTRL_0);
	(void)readl(mdsi->regs + DSI_CTRL_0);
	writel(DSI_CTRL_0_SOFT_RST | DSI_CTRL_0_SOFT_RST_REG,
	       mdsi->regs + DSI_CTRL_0);
	usleep_range(1000, 2000);
	writel(0, mdsi->regs + DSI_PHY_CTRL_1);
	writel(0, mdsi->regs + DSI_CTRL_0);
	usleep_range(1000, 2000);

	writel(0, mdsi->regs + DSI_IRQ_MASK);
	writel(0x000000e4, mdsi->regs + DSI_CTRL_1);
	writel(0, mdsi->regs + DSI_PHY_CTRL_3);
	mmp_dsi_set_dphy_timing(mdsi);

	writel(0x00066666, mdsi->regs + DSI_MEM_CTRL);
	writel(0xffffffff, mdsi->regs + DSI_TX_TIMER);
	writel(0xffffffff, mdsi->regs + DSI_RX_TIMER);
	writel(0xffffffff, mdsi->regs + DSI_TURN_TIMER);

	mmp_dsi_set_video_timings(mdsi);

	writel(0x1, mdsi->regs + DSI_PHY_CTRL_1);
	writel(0x0, mdsi->regs + DSI_PHY_CTRL_2);
	usleep_range(10, 20);
	writel(0x10, mdsi->regs + DSI_PHY_CTRL_2);
	writel(DSI_CPU_CMD_1_LPDT(0x1), mdsi->regs + DSI_CPU_CMD_1);
	usleep_range(1000, 2000);

	mdsi->active_lanes = 1;
	mdsi->lp_ready = true;
	return 0;
}

static void mmp_dsi_power_off(struct mmp_dsi *mdsi)
{
	if (!mdsi->lp_ready)
		return;

	writel(0, mdsi->regs + DSI_CTRL_0);
	writel(0, mdsi->regs + DSI_PHY_CTRL_2);
	writel(0, mdsi->regs + DSI_PHY_CTRL_1);

	clk_disable_unprepare(mdsi->clk_dsi_phy);
	clk_disable_unprepare(mdsi->clk_dsi_esc);

	mdsi->video_enabled = false;
	mdsi->lp_ready = false;
	mdsi->active_lanes = 0;
}

static int mmp_dsi_write_fifo(struct mmp_dsi *mdsi, const u8 *buf, size_t len)
{
	size_t i;
	u32 val;
	int ret;

	for (i = 0; i < len; i += 4) {
		size_t rem = min_t(size_t, 4, len - i);
		u32 word = 0;
		size_t b;

		for (b = 0; b < rem; b++)
			word |= (u32)buf[i + b] << (b * 8);

		writel(word, mdsi->regs + DSI_CPU_WDAT_0);
		writel(DSI_CPU_DAT_REQ | DSI_CPU_DAT_RW | DSI_CPU_DAT_ADDR(i),
		       mdsi->regs + DSI_CPU_CMD_3);

		ret = readl_poll_timeout_atomic(mdsi->regs + DSI_CPU_CMD_3, val,
						!(val & DSI_CPU_DAT_REQ),
						1, 1000);
		if (ret)
			return ret;
	}

	return 0;
}

static ssize_t mmp_dsi_read_rx(struct mmp_dsi *mdsi, struct mipi_dsi_msg *msg)
{
	u32 st, pkt_st, ctrl1, reg;
	u8 rx_buf[64];
	u32 ptr, bcnt, i;
	int ret;

	ret = readl_poll_timeout(mdsi->regs + DSI_IRQ_ST, st,
				 st & DSI_IRQ_RX_PKT, 10, 50000);
	if (ret)
		return ret;

	pkt_st = readl(mdsi->regs + DSI_RX_PKT_ST_0);
	ptr = (pkt_st >> 16) & 0xf;
	ctrl1 = readl(mdsi->regs + DSI_RX_PKT_CTRL_1);
	bcnt = min_t(u32, ctrl1 & 0xff, sizeof(rx_buf));

	for (i = 0; i < bcnt; i++) {
		reg = readl(mdsi->regs + DSI_RX_PKT_CTRL);
		reg &= ~GENMASK(21, 16);
		reg |= BIT(31) | ((ptr + i) << 16);
		writel(reg, mdsi->regs + DSI_RX_PKT_CTRL);

		ret = readl_poll_timeout_atomic(mdsi->regs + DSI_RX_PKT_CTRL,
						reg, !(reg & BIT(31)), 1, 1000);
		if (ret)
			return ret;

		rx_buf[i] = reg & 0xff;
	}

	if (bcnt < 3)
		return -EIO;

	switch (rx_buf[0]) {
	case MIPI_DSI_RX_GENERIC_SHORT_READ_RESPONSE_1BYTE:
	case MIPI_DSI_RX_DCS_SHORT_READ_RESPONSE_1BYTE:
		bcnt = min_t(size_t, msg->rx_len, 1);
		memcpy(msg->rx_buf, &rx_buf[1], bcnt);
		return bcnt;
	case MIPI_DSI_RX_GENERIC_SHORT_READ_RESPONSE_2BYTE:
	case MIPI_DSI_RX_DCS_SHORT_READ_RESPONSE_2BYTE:
		bcnt = min_t(size_t, msg->rx_len, 2);
		memcpy(msg->rx_buf, &rx_buf[1], bcnt);
		return bcnt;
	case MIPI_DSI_RX_GENERIC_LONG_READ_RESPONSE:
	case MIPI_DSI_RX_DCS_LONG_READ_RESPONSE:
		if (bcnt < 4)
			return -EIO;
		bcnt = min_t(size_t, msg->rx_len, bcnt - 4);
		memcpy(msg->rx_buf, &rx_buf[4], bcnt);
		return bcnt;
	default:
		return -EIO;
	}
}

static ssize_t mmp_dsi_host_transfer(struct mipi_dsi_host *host,
				     const struct mipi_dsi_msg *msg)
{
	struct mmp_dsi *mdsi = host_to_mmp_dsi(host);
	struct mipi_dsi_packet pkt;
	u8 tx_raw[MMP_DSI_MAX_TX_BYTES];
	u8 tx_wire[MMP_DSI_MAX_TX_BYTES];
	const u8 *tx_buf;
	bool lpm = !!(msg->flags & MIPI_DSI_MSG_USE_LPM);
	bool turn = msg->rx_len > 0;
	bool is_short;
	size_t raw_len, wire_len;
	u32 cmd0, wc, val;
	ssize_t ret;

	ret = mipi_dsi_create_packet(&pkt, msg);
	if (ret)
		return ret;

	pkt.header[3] = mmp_dsi_ecc(pkt.header);
	is_short = !mipi_dsi_packet_format_is_long(msg->type);

	raw_len = is_short ? 4 : (4 + pkt.payload_length + 2);
	if (raw_len > sizeof(tx_raw))
		return -EMSGSIZE;

	memcpy(tx_raw, pkt.header, 4);
	if (!is_short) {
		u16 crc = mmp_dsi_crc16(pkt.payload, pkt.payload_length);

		memcpy(&tx_raw[4], pkt.payload, pkt.payload_length);
		tx_raw[4 + pkt.payload_length] = crc & 0xff;
		tx_raw[5 + pkt.payload_length] = crc >> 8;
	}

	mutex_lock(&mdsi->tx_mutex);

	ret = mmp_dsi_ensure_lp(mdsi);
	if (ret)
		goto out_unlock;

	/*
	 * When transmitting LPM packets while multiple lanes are active, the
	 * controller interleaves TX FIFO bytes across lanes so each byte must
	 * be replicated across active_lanes. In 1-lane LP cold init no copy
	 * is needed.
	 */
	if (lpm && mdsi->active_lanes > 1) {
		size_t i, j;

		wire_len = raw_len * mdsi->active_lanes;
		if (wire_len > sizeof(tx_wire)) {
			ret = -EMSGSIZE;
			goto out_unlock;
		}
		for (i = 0; i < raw_len; i++) {
			for (j = 0; j < mdsi->active_lanes; j++)
				tx_wire[i * mdsi->active_lanes + j] = tx_raw[i];
		}
		tx_buf = tx_wire;
	} else {
		wire_len = raw_len;
		tx_buf = tx_raw;
	}

	ret = mmp_dsi_write_fifo(mdsi, tx_buf, wire_len);
	if (ret)
		goto out_unlock;

	wc = lpm ? wire_len : (is_short ? wire_len : wire_len - 2);
	cmd0 = DSI_CPU_CMD_REQ | (wc & DSI_CPU_CMD_WC_MASK);
	if (is_short)
		cmd0 |= DSI_CPU_CMD_SP;
	if (turn) {
		cmd0 |= DSI_CPU_CMD_TURN;
		(void)readl(mdsi->regs + DSI_IRQ_ST);
	}
	if (lpm)
		cmd0 |= DSI_CPU_CMD_TXLP;

	writel(cmd0, mdsi->regs + DSI_CPU_CMD_0);

	ret = readl_poll_timeout(mdsi->regs + DSI_CPU_CMD_0, val,
				 !(val & DSI_CPU_CMD_REQ), 10, 50000);
	if (ret) {
		dev_err(mdsi->dev, "DSI CPU packet timeout (cmd0=0x%08x)\n",
			cmd0);
		goto out_unlock;
	}

	if (turn)
		ret = mmp_dsi_read_rx(mdsi, (struct mipi_dsi_msg *)msg);
	else
		ret = msg->tx_len;

out_unlock:
	mutex_unlock(&mdsi->tx_mutex);
	return ret;
}

static const struct component_ops mmp_dsi_component_ops;

static int mmp_dsi_host_attach(struct mipi_dsi_host *host,
			       struct mipi_dsi_device *device)
{
	struct mmp_dsi *mdsi = host_to_mmp_dsi(host);

	if (device->lanes < 1 || device->lanes > 4)
		return -EINVAL;

	mdsi->lanes = device->lanes;
	mdsi->format = device->format;

	return component_add(mdsi->dev, &mmp_dsi_component_ops);
}

static int mmp_dsi_host_detach(struct mipi_dsi_host *host,
			       struct mipi_dsi_device *device)
{
	struct mmp_dsi *mdsi = host_to_mmp_dsi(host);

	component_del(mdsi->dev, &mmp_dsi_component_ops);
	return 0;
}

static const struct mipi_dsi_host_ops mmp_dsi_host_ops = {
	.attach = mmp_dsi_host_attach,
	.detach = mmp_dsi_host_detach,
	.transfer = mmp_dsi_host_transfer,
};

static void mmp_dsi_encoder_mode_set(struct drm_encoder *encoder,
				     struct drm_display_mode *mode,
				     struct drm_display_mode *adjusted_mode)
{
	struct mmp_dsi *mdsi = enc_to_mmp_dsi(encoder);

	drm_mode_copy(&mdsi->mode, adjusted_mode);
}

static void mmp_dsi_encoder_enable(struct drm_encoder *encoder)
{
	struct mmp_dsi *mdsi = enc_to_mmp_dsi(encoder);
	u32 lane_mask;
	int ret;

	mutex_lock(&mdsi->tx_mutex);

	ret = mmp_dsi_ensure_lp(mdsi);
	if (ret) {
		dev_err(mdsi->dev, "failed to power on DSI PHY: %d\n", ret);
		mutex_unlock(&mdsi->tx_mutex);
		return;
	}

	mmp_dsi_set_video_timings(mdsi);

	lane_mask = GENMASK(mdsi->lanes - 1, 0);
	writel(0, mdsi->regs + DSI_PHY_CTRL_2);
	usleep_range(10, 20);
	writel(lane_mask << 4, mdsi->regs + DSI_PHY_CTRL_2);
	writel(DSI_CPU_CMD_1_LPDT(lane_mask), mdsi->regs + DSI_CPU_CMD_1);
	usleep_range(1000, 2000);
	writel(DSI_CTRL_0_LCD1_TX_EN | DSI_CTRL_0_LCD1_EN,
	       mdsi->regs + DSI_CTRL_0);
	(void)readl(mdsi->regs + DSI_CTRL_0);

	mdsi->active_lanes = mdsi->lanes;
	mdsi->video_enabled = true;

	mutex_unlock(&mdsi->tx_mutex);
}

static void mmp_dsi_encoder_disable(struct drm_encoder *encoder)
{
	struct mmp_dsi *mdsi = enc_to_mmp_dsi(encoder);

	mutex_lock(&mdsi->tx_mutex);
	mmp_dsi_power_off(mdsi);
	mutex_unlock(&mdsi->tx_mutex);
}

static const struct drm_encoder_helper_funcs mmp_dsi_encoder_helper_funcs = {
	.mode_set = mmp_dsi_encoder_mode_set,
	.enable = mmp_dsi_encoder_enable,
	.disable = mmp_dsi_encoder_disable,
};

static const struct drm_encoder_funcs mmp_dsi_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

static int mmp_dsi_bind(struct device *dev, struct device *master, void *data)
{
	struct mmp_dsi *mdsi = dev_get_drvdata(dev);
	struct drm_device *drm = data;
	struct drm_panel *panel = NULL;
	struct drm_bridge *bridge = NULL;
	int ret;

	ret = drm_of_find_panel_or_bridge(dev->of_node, 1, 0, &panel, &bridge);
	if (ret)
		return ret;

	if (panel) {
		bridge = drm_panel_bridge_add(panel, DRM_MODE_CONNECTOR_DSI);
		if (IS_ERR(bridge))
			return PTR_ERR(bridge);
		mdsi->panel_bridge = bridge;
	} else {
		mdsi->panel_bridge = NULL;
	}

	mdsi->bridge = bridge;
	mdsi->encoder.possible_crtcs = BIT(0);
	ret = drm_encoder_init(drm, &mdsi->encoder, &mmp_dsi_encoder_funcs,
			       DRM_MODE_ENCODER_DSI, NULL);
	if (ret)
		goto err_panel_bridge;

	drm_encoder_helper_add(&mdsi->encoder, &mmp_dsi_encoder_helper_funcs);

	ret = drm_bridge_attach(&mdsi->encoder, bridge, NULL);
	if (ret)
		goto err_encoder;

	return 0;

err_encoder:
	drm_encoder_cleanup(&mdsi->encoder);
err_panel_bridge:
	if (mdsi->panel_bridge) {
		drm_panel_bridge_remove(mdsi->panel_bridge);
		mdsi->panel_bridge = NULL;
	}
	return ret;
}

static void mmp_dsi_unbind(struct device *dev, struct device *master, void *data)
{
	struct mmp_dsi *mdsi = dev_get_drvdata(dev);

	drm_encoder_cleanup(&mdsi->encoder);
	if (mdsi->panel_bridge) {
		drm_panel_bridge_remove(mdsi->panel_bridge);
		mdsi->panel_bridge = NULL;
	}
}

static const struct component_ops mmp_dsi_component_ops = {
	.bind = mmp_dsi_bind,
	.unbind = mmp_dsi_unbind,
};

static int mmp_dsi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mmp_dsi *mdsi;
	struct resource *res;

	mdsi = devm_kzalloc(dev, sizeof(*mdsi), GFP_KERNEL);
	if (!mdsi)
		return -ENOMEM;

	mdsi->dev = dev;
	mdsi->lanes = 4;
	mdsi->format = MIPI_DSI_FMT_RGB888;
	mutex_init(&mdsi->tx_mutex);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	mdsi->regs = devm_ioremap_resource(dev, res);
	if (IS_ERR(mdsi->regs))
		return PTR_ERR(mdsi->regs);

	mdsi->clk_dsi_phy = devm_clk_get(dev, "dsi_phy");
	if (IS_ERR(mdsi->clk_dsi_phy))
		return PTR_ERR(mdsi->clk_dsi_phy);

	mdsi->clk_dsi_esc = devm_clk_get(dev, "dsi_esc");
	if (IS_ERR(mdsi->clk_dsi_esc))
		return PTR_ERR(mdsi->clk_dsi_esc);

	platform_set_drvdata(pdev, mdsi);

	mdsi->host.dev = dev;
	mdsi->host.ops = &mmp_dsi_host_ops;
	return mipi_dsi_host_register(&mdsi->host);
}

static int mmp_dsi_remove(struct platform_device *pdev)
{
	struct mmp_dsi *mdsi = platform_get_drvdata(pdev);

	mipi_dsi_host_unregister(&mdsi->host);
	return 0;
}

static const struct of_device_id mmp_dsi_dt_ids[] = {
	{ .compatible = "marvell,pxa988-dsi", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mmp_dsi_dt_ids);

struct platform_driver mmp_dsi_driver = {
	.probe = mmp_dsi_probe,
	.remove = mmp_dsi_remove,
	.driver = {
		.name = "mmp-dsi",
		.of_match_table = mmp_dsi_dt_ids,
	},
};
