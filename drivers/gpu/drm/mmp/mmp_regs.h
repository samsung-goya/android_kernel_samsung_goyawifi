/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Register definitions for Marvell PXA988/PXA986 LCDC and MIPI DSI host.
 */
#ifndef __MMP_REGS_H__
#define __MMP_REGS_H__

/* LCD Controller (0xd420b000) offsets */
#define LCD_SPU_DMA_START_ADDR_Y0	0x00c0
#define LCD_SPU_DMA_START_ADDR_U0	0x00c4
#define LCD_SPU_DMA_START_ADDR_V0	0x00c8
#define LCD_SPU_DMA_START_ADDR_C0	0x00cc
#define LCD_SPU_DMA_START_ADDR_Y1	0x00d0
#define LCD_SPU_DMA_START_ADDR_U1	0x00d4
#define LCD_SPU_DMA_START_ADDR_V1	0x00d8
#define LCD_SPU_DMA_START_ADDR_C1	0x00dc
#define LCD_SPU_DMA_PITCH_YC		0x00e0
#define LCD_SPU_DMA_PITCH_UV		0x00e4
#define LCD_SPU_DMA_OVSA_HPXL_VLN	0x00e8
#define LCD_SPU_DMA_HPXL_VLN		0x00ec
#define LCD_SPU_DZM_HPXL_VLN		0x00f0

#define LCD_CFG_GRA_START_ADDR0		0x00f4
#define LCD_CFG_GRA_START_ADDR1		0x00f8
#define LCD_CFG_GRA_PITCH		0x00fc
#define LCD_SPU_GRA_OVSA_HPXL_VLN	0x0100
#define LCD_SPU_GRA_HPXL_VLN		0x0104
#define LCD_SPU_GZM_HPXL_VLN		0x0108

#define LCD_SPU_V_H_TOTAL		0x0114
#define LCD_SPU_V_H_ACTIVE		0x0118
#define LCD_SPU_H_PORCH			0x011c
#define LCD_SPU_V_PORCH			0x0120
#define LCD_SPU_BLANKCOLOR		0x0124
#define LCD_PN_SEPXLCNT			0x013c
#define LCD_SPU_SPI_CTRL		0x0180
#define LCD_SPU_SMPN_CTRL		0x0188

#define LCD_SPU_DMA_CTRL0		0x0190
#define  CFG_ARBFAST_ENA		BIT(27)
#define  CFG_DMAFORMAT(x)		((x) << 20)
#define  CFG_DMAFORMAT_MASK		GENMASK(23, 20)
#define  CFG_GRAFORMAT(x)		((x) << 16)
#define  CFG_GRAFORMAT_MASK		GENMASK(19, 16)
#define  CFG_GRA_FTOGGLE		BIT(15)
#define  CFG_GRA_HSMOOTH		BIT(14)
#define  CFG_GRA_TSTMODE		BIT(13)
#define  CFG_GRA_SWAPRB			BIT(12)
#define  CFG_GRA_SWAPUV			BIT(11)
#define  CFG_GRA_SWAPYU			BIT(10)
#define  CFG_GRA_SWAP_MASK		GENMASK(12, 10)
#define  CFG_YUV2RGB_GRA		BIT(9)
#define  CFG_GRA_ENA			BIT(8)
#define  CFG_DMA_FTOGGLE		BIT(7)
#define  CFG_DMA_HSMOOTH		BIT(6)
#define  CFG_DMA_TSTMODE		BIT(5)
#define  CFG_DMA_SWAPRB			BIT(4)
#define  CFG_DMA_SWAPUV			BIT(3)
#define  CFG_DMA_SWAPYU			BIT(2)
#define  CFG_DMA_SWAP_MASK		GENMASK(4, 2)
#define  CFG_YUV2RGB_DMA		BIT(1)
#define  CFG_DMA_ENA			BIT(0)
#define  CFG_GRA_ALL_MASK		(CFG_GRAFORMAT_MASK | CFG_GRA_FTOGGLE | \
					 CFG_GRA_HSMOOTH | CFG_GRA_TSTMODE | \
					 CFG_GRA_SWAP_MASK | CFG_YUV2RGB_GRA | \
					 CFG_GRA_ENA)
#define  CFG_DMA_ALL_MASK		(CFG_DMAFORMAT_MASK | CFG_DMA_FTOGGLE | \
					 CFG_DMA_HSMOOTH | CFG_DMA_TSTMODE | \
					 CFG_DMA_SWAP_MASK | CFG_YUV2RGB_DMA | \
					 CFG_DMA_ENA)
#define  CFG_NOBLENDING			BIT(31)
#define  CFG_GAMMA_ENA			BIT(30)
#define  CFG_CBSH_ENA			BIT(29)
#define  CFG_PALETTE_ENA		BIT(28)

#define LCD_SPU_DMA_CTRL1		0x0194
#define  CFG_FRAME_TRIG			BIT(31)
#define  CFG_VSYNC_TRIG(x)		((x) << 28)
#define  CFG_VSYNC_TRIG_MASK		GENMASK(30, 28)
#define  CFG_VSYNC_INV			BIT(27)
#define  CFG_COLOR_KEY_MODE(x)		((x) << 24)
#define  CFG_COLOR_KEY_MASK		GENMASK(26, 24)
#define  CFG_LNBUF_ENA			BIT(22)
#define  CFG_GATED_ENA			BIT(21)
#define  CFG_PWRDN_ENA			BIT(20)
#define  CFG_ALPHA_MODE(x)		((x) << 16)
#define  CFG_ALPHA_MODE_MASK		GENMASK(17, 16)
#define  CFG_ALPHAM_VIDEO		CFG_ALPHA_MODE(0)
#define  CFG_ALPHAM_GRA			CFG_ALPHA_MODE(1)
#define  CFG_ALPHAM_CFG			CFG_ALPHA_MODE(2)
#define  CFG_ALPHA(x)			((x) << 8)
#define  CFG_ALPHA_MASK			GENMASK(15, 8)
#define  CFG_PXLCMD(x)			((x) << 0)

#define LCD_SPU_SRAM_CTRL		0x0198
#define  SRAM_READ			(0 << 14)
#define  SRAM_WRITE			(2 << 14)
#define  SRAM_INIT			(3 << 14)
#define  SRAM_ADDR_LCDID(x)		((x) << 8)
#define  SRAM_GAMMA_YR			0x0
#define  SRAM_GAMMA_UG			0x1
#define  SRAM_GAMMA_VB			0x2
#define LCD_SPU_SRAM_WRDAT		0x019c
#define LCD_SPU_SRAM_PARA0		0x01a0
#define LCD_SPU_SRAM_PARA1		0x01a4
#define  CFG_CSB_256x8			BIT(13)
#define  CFG_PDWN256x8			BIT(5)
#define  CFG_PDWN32x32			BIT(3)
#define  CFG_PDWN16x66			BIT(2)
#define  CFG_PDWN32x66			BIT(1)
#define  CFG_PDWN64x66			BIT(0)

#define LCD_CFG_SCLK_DIV		0x01a8
#define  SCLK_SRC_DISP1			(0x1 << 30)
#define  SCLK_SRC_DSI_PLL		(0x3 << 30)
#define  SCLK_SRC_MASK			GENMASK(31, 30)
#define  SCLK_DISABLE			BIT(28)
#define  SCLK_DSI_SEL			BIT(12)
#define  SCLK_DSI1_DIV(x)		((x) << 8)
#define  SCLK_DSI1_DIV_MASK		GENMASK(11, 8)
#define  SCLK_INT_DIV(x)		((x) << 0)
#define  SCLK_INT_DIV_MASK		GENMASK(7, 0)

#define LCD_SPU_DUMB_CTRL		0x01b8
#define  CFG_DUMBMODE(x)		((x) << 28)
#define  CFG_DUMBMODE_MASK		GENMASK(31, 28)
#define  CFG_INV_VSYNC			BIT(3)
#define  CFG_INV_HSYNC			BIT(2)
#define  CFG_INV_PCLK			BIT(1)
#define  CFG_DUMB_ENA			BIT(0)

#define LCD_SPU_IOPAD_CONTROL		0x01bc
#define  CFG_CSC_MASK			GENMASK(9, 8)
#define  CFG_CSC_YUV_CCIR709		BIT(9)
#define  CFG_CSC_YUV_CCIR601		0
#define  CFG_CSC_RGB_STUDIO		BIT(8)
#define  CFG_CSC_RGB_COMPUTER		0
#define  CFG_CYC_BURST_LEN16		BIT(4)

#define LCD_SPU_IRQ_ENA			0x01c0
#define LCD_SPU_IRQ_ISR			0x01c4
#define  GRA_FRAME_IRQ0			BIT(27)
#define  GRA_FRAME_IRQ1			BIT(26)
#define  GRA_FF_UNDERFLOW		BIT(25)
#define  VSYNC_IRQ			BIT(23)
#define  DUMB_FRAMEDONE			BIT(22)
#define  DMA_FF_UNDERFLOW		BIT(17)

#define LCD_SPU_GRA_CUTHPXL		0x01cc
#define LCD_SPU_GRA_CUTVLN		0x01d0
#define LCD_TOP_CTRL			0x01dc
#define LCD_SQULN1_CTRL			0x01e0
#define LCD_SQULN2_CTRL			0x01e4
#define LCD_AFA_ALL2ONE			0x01e8
#define  AFA_DMA_TWO_ZOOM_PN		BIT(20)

#define TIMING_MASTER_CONTROL		0x01f4
#define  MASTER_ENV(id)			BIT((id) + 6)
#define  MASTER_ENH(id)			BIT((id) + 4)
#define  YUV420SP_SWAPUV_PN		BIT(0)

/* MIPI DSI Host (0xd420b800) offsets */
#define DSI_CTRL_0			0x0000
#define  DSI_CTRL_0_SOFT_RST		BIT(31)
#define  DSI_CTRL_0_SOFT_RST_REG	BIT(30)
#define  DSI_CTRL_0_LCD1_TX_EN		BIT(8)
#define  DSI_CTRL_0_LCD1_SLV		BIT(4)
#define  DSI_CTRL_0_LCD1_EN		BIT(0)

#define DSI_CTRL_1			0x0004
#define DSI_IRQ_ST			0x0010
#define  DSI_IRQ_CPU_TX_DONE		BIT(0)
#define  DSI_IRQ_RX_PKT			BIT(2)
#define DSI_IRQ_MASK			0x0014

#define DSI_CPU_CMD_0			0x0020
#define  DSI_CPU_CMD_REQ		BIT(31)
#define  DSI_CPU_CMD_SP			BIT(30)
#define  DSI_CPU_CMD_TURN		BIT(29)
#define  DSI_CPU_CMD_TXLP		BIT(27)
#define  DSI_CPU_CMD_WC_MASK		GENMASK(7, 0)

#define DSI_CPU_CMD_1			0x0024
#define  DSI_CPU_CMD_1_LPDT(mask)	((mask) << 20)

#define DSI_CPU_CMD_3			0x002c
#define  DSI_CPU_DAT_REQ		BIT(31)
#define  DSI_CPU_DAT_RW			BIT(30)
#define  DSI_CPU_DAT_ADDR(x)		((x) << 16)

#define DSI_CPU_WDAT_0			0x0030
#define DSI_RX_PKT_ST_0			0x0060
#define DSI_RX_PKT_HDR_0		0x0064
#define DSI_RX_PKT_CTRL			0x0070
#define DSI_RX_PKT_CTRL_1		0x0074

#define DSI_PHY_CTRL_1			0x0084
#define DSI_PHY_CTRL_2			0x0088
#define DSI_PHY_CTRL_3			0x008c
#define DSI_PHY_RCOMP_0			0x00b0
#define DSI_PHY_TIME_0			0x00c0
#define DSI_PHY_TIME_1			0x00c4
#define DSI_PHY_TIME_2			0x00c8
#define DSI_PHY_TIME_3			0x00cc
#define DSI_MEM_CTRL			0x00e0
#define DSI_TX_TIMER			0x00e4
#define DSI_RX_TIMER			0x00e8
#define DSI_TURN_TIMER			0x00ec

#define DSI_LCD1_CTRL_0			0x0100
#define DSI_LCD1_CTRL_1			0x0104
#define  DSI_LCD1_VSYNC_RST_EN		BIT(31)
#define  DSI_LCD1_HBP_PKT_EN		BIT(18)
#define  DSI_LCD1_HFP_PKT_EN		BIT(20)
#define  DSI_LCD1_BURST_SYNC_EVENT	BIT(2)
#define  DSI_LCD1_RGB888		0x3

#define DSI_LCD1_TIMING_0		0x0110
#define DSI_LCD1_TIMING_1		0x0114
#define DSI_LCD1_TIMING_2		0x0118
#define DSI_LCD1_TIMING_3		0x011c
#define DSI_LCD1_WC_0			0x0120
#define DSI_LCD1_WC_1			0x0124
#define DSI_LCD1_WC_2			0x0128

#endif /* __MMP_REGS_H__ */
