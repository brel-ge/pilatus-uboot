// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright 2019 NXP
 */

#include <common.h>
#include <malloc.h>
#include <video.h>
#include <video_bridge.h>
#include <video_link.h>

#include <asm/cache.h>
#include <asm/arch/imx-regs.h>
#include <asm/arch/clock.h>
#include <asm/arch/sys_proto.h>
#include <linux/err.h>
#include <asm/io.h>

#include "../../videomodes.h"
#include <linux/string.h>
#include <linux/list.h>
#include <linux/fb.h>
#include <linux/bug.h>
#include <linux/delay.h>
#include "lcdifv3-regs.h"
#include <log.h>
#include <dm.h>
#include <dm/device-internal.h>
#include <dm/device_compat.h>

#define	PS2KHZ(ps)	(1000000000UL / (ps))
#define HZ2PS(hz)	(1000000000UL / ((hz) / 1000))

struct lcdifv3_priv {
	fdt_addr_t reg_base;
	struct udevice *disp_dev;

	u32 thres_low_mul;
	u32 thres_low_div;
	u32 thres_high_mul;
	u32 thres_high_div;
};

static int lcdifv3_set_pix_fmt(struct lcdifv3_priv *priv, unsigned int format)
{
	uint32_t ctrldescl0_5 = 0;

	ctrldescl0_5 = readl((ulong)(priv->reg_base + LCDIFV3_CTRLDESCL0_5));

	WARN_ON(ctrldescl0_5 & CTRLDESCL0_5_SHADOW_LOAD_EN);

	ctrldescl0_5 &= ~(CTRLDESCL0_5_BPP(0xf) | CTRLDESCL0_5_YUV_FORMAT(0x3));

	switch (format) {
	case VIDEO_BPP16:
		ctrldescl0_5 |= CTRLDESCL0_5_BPP(BPP16_RGB565);
		break;
	case VIDEO_BPP32:
		ctrldescl0_5 |= CTRLDESCL0_5_BPP(BPP32_ARGB8888);
		break;
	default:
		printf("unsupported pixel format: %u\n", format);
		return -EINVAL;
	}

	writel(ctrldescl0_5,  (ulong)(priv->reg_base + LCDIFV3_CTRLDESCL0_5));

	return 0;
}


static void lcdifv3_set_mode(struct lcdifv3_priv *priv,
			struct ctfb_res_modes *mode)
{
	u32 disp_size, hsyn_para, vsyn_para, vsyn_hsyn_width, ctrldescl0_1;

	/* config display timings */
	disp_size = DISP_SIZE_DELTA_Y(mode->yres) |
		    DISP_SIZE_DELTA_X(mode->xres);
	writel(disp_size, (ulong)(priv->reg_base + LCDIFV3_DISP_SIZE));

	hsyn_para = HSYN_PARA_BP_H(mode->left_margin) |
		    HSYN_PARA_FP_H(mode->right_margin);
	writel(hsyn_para, (ulong)(priv->reg_base + LCDIFV3_HSYN_PARA));

	vsyn_para = VSYN_PARA_BP_V(mode->upper_margin) |
		    VSYN_PARA_FP_V(mode->lower_margin);
	writel(vsyn_para, (ulong)(priv->reg_base + LCDIFV3_VSYN_PARA));

	vsyn_hsyn_width = VSYN_HSYN_WIDTH_PW_V(mode->vsync_len) |
			  VSYN_HSYN_WIDTH_PW_H(mode->hsync_len);
	writel(vsyn_hsyn_width, (ulong)(priv->reg_base + LCDIFV3_VSYN_HSYN_WIDTH));

	/* config layer size */
	/* TODO: 32bits alignment for width */
	ctrldescl0_1 = CTRLDESCL0_1_HEIGHT(mode->yres) |
		       CTRLDESCL0_1_WIDTH(mode->xres);
	writel(ctrldescl0_1, (ulong)(priv->reg_base + LCDIFV3_CTRLDESCL0_1));

	/* Polarities */
	if (mode->sync & FB_SYNC_VERT_HIGH_ACT)
		writel(CTRL_INV_VS, (ulong)(priv->reg_base + LCDIFV3_CTRL_CLR));
	else
		writel(CTRL_INV_VS, (ulong)(priv->reg_base + LCDIFV3_CTRL_SET));

	if (mode->sync & FB_SYNC_HOR_HIGH_ACT)
		writel(CTRL_INV_HS, (ulong)(priv->reg_base + LCDIFV3_CTRL_CLR));
	else
		writel(CTRL_INV_HS, (ulong)(priv->reg_base + LCDIFV3_CTRL_SET));

	/* SEC MIPI DSI specific */
	writel(CTRL_INV_PXCK, (ulong)(priv->reg_base + LCDIFV3_CTRL_CLR));
	writel(CTRL_INV_DE, (ulong)(priv->reg_base + LCDIFV3_CTRL_CLR));

}

static void lcdifv3_set_bus_fmt(struct lcdifv3_priv *priv)
{
	uint32_t disp_para = 0;

	disp_para = readl((ulong)(priv->reg_base + LCDIFV3_DISP_PARA));
	disp_para &= DISP_PARA_LINE_PATTERN(0xf);

	/* Fixed to 24 bits output */
	disp_para |= DISP_PARA_LINE_PATTERN(LP_RGB888_OR_YUV444);

	/* config display mode: default is normal mode */
	disp_para &= DISP_PARA_DISP_MODE(3);
	disp_para |= DISP_PARA_DISP_MODE(0);
	writel(disp_para, (ulong)(priv->reg_base + LCDIFV3_DISP_PARA));
}

static void lcdifv3_enable_plane_panic(struct lcdifv3_priv *priv)
{
	u32 panic_thres, thres_low, thres_high;

	/* apb clock has been enabled */

	/* As suggestion, the thres_low should be 1/3 FIFO,
	 * and thres_high should be 2/3 FIFO (The FIFO size
	 * is 8KB = 512 * 128bit).
	 * threshold = n * 128bit (n: 0 ~ 511)
	 */
	thres_low  = DIV_ROUND_UP(511 * priv->thres_low_mul,
			priv->thres_low_div);
	thres_high = DIV_ROUND_UP(511 * priv->thres_high_mul,
			priv->thres_high_div);

	panic_thres = PANIC0_THRES_PANIC_THRES_LOW(thres_low)	|
		      PANIC0_THRES_PANIC_THRES_HIGH(thres_high);

	writel(panic_thres, priv->reg_base + LCDIFV3_PANIC0_THRES);

	/* Enable Panic:
	 *
	 * As designed, the panic won't trigger an irq,
	 * so it is unnecessary to handle this as an irq
	 * and NoC + QoS modules will handle panic
	 * automatically.
	 */
	writel(INT_ENABLE_D1_PLANE_PANIC_EN,
	       priv->reg_base + LCDIFV3_INT_ENABLE_D1);
}

static void lcdifv3_enable_controller(struct lcdifv3_priv *priv)
{
	u32 disp_para, ctrldescl0_5;

	disp_para = readl((ulong)(priv->reg_base + LCDIFV3_DISP_PARA));
	ctrldescl0_5 = readl((ulong)(priv->reg_base + LCDIFV3_CTRLDESCL0_5));

	/* disp on */
	disp_para |= DISP_PARA_DISP_ON;
	writel(disp_para, (ulong)(priv->reg_base + LCDIFV3_DISP_PARA));

	/* enable shadow load */
	ctrldescl0_5 |= CTRLDESCL0_5_SHADOW_LOAD_EN;
	writel(ctrldescl0_5, (ulong)(priv->reg_base + LCDIFV3_CTRLDESCL0_5));

	/* enable layer dma */
	ctrldescl0_5 |= CTRLDESCL0_5_EN;
	writel(ctrldescl0_5, (ulong)(priv->reg_base + LCDIFV3_CTRLDESCL0_5));
}

static void lcdifv3_disable_controller(struct lcdifv3_priv *priv)
{
	u32 disp_para, ctrldescl0_5;

	disp_para = readl((ulong)(priv->reg_base + LCDIFV3_DISP_PARA));
	ctrldescl0_5 = readl((ulong)(priv->reg_base + LCDIFV3_CTRLDESCL0_5));

	/* dma off */
	ctrldescl0_5 &= ~CTRLDESCL0_5_EN;
	writel(ctrldescl0_5, (ulong)(priv->reg_base + LCDIFV3_CTRLDESCL0_5));

	/* disp off */
	disp_para &= ~DISP_PARA_DISP_ON;
	writel(disp_para, (ulong)(priv->reg_base + LCDIFV3_DISP_PARA));
}

static void print_ctfb_res_modes(struct ctfb_res_modes *mode) {
    printf("xres: %d\n", mode->xres);
    printf("yres: %d\n", mode->yres);
    printf("refresh: %d Hz\n", mode->refresh);
    printf("pixclock: %d ps\n", mode->pixclock);
    printf("pixclock_khz: %d kHz\n", mode->pixclock_khz);
    printf("left_margin: %d\n", mode->left_margin);
    printf("right_margin: %d\n", mode->right_margin);
    printf("upper_margin: %d\n", mode->upper_margin);
    printf("lower_margin: %d\n", mode->lower_margin);
    printf("hsync_len: %d\n", mode->hsync_len);
    printf("vsync_len: %d\n", mode->vsync_len);
    printf("sync: 0x%X\n", mode->sync);
    printf("vmode: 0x%X\n", mode->vmode);
}

static void lcdifv3_init(struct udevice *dev,
			struct ctfb_res_modes *mode, unsigned int format)
{
	struct video_uc_plat *plat = dev_get_uclass_plat(dev);
	struct lcdifv3_priv *priv = dev_get_priv(dev);
	int ret;

  print_ctfb_res_modes(mode);

	/* Kick in the LCDIF clock */
	mxs_set_lcdclk(priv->reg_base, PS2KHZ(mode->pixclock));

	writel(CTRL_SW_RESET, (ulong)(priv->reg_base + LCDIFV3_CTRL_CLR));

	/* enable plane FIFO panic */
	lcdifv3_enable_plane_panic(priv);

	lcdifv3_set_mode(priv, mode);

	lcdifv3_set_bus_fmt(priv);

	ret = lcdifv3_set_pix_fmt(priv, format);
	if (ret) {
		printf("Fail to init lcdifv3, wrong format %u\n", format);
		return;
	}

	/* Set fb address to primary layer */
	writel(plat->base, (ulong)(priv->reg_base + LCDIFV3_CTRLDESCL_LOW0_4));

	writel(CTRLDESCL0_3_P_SIZE(1) |CTRLDESCL0_3_T_SIZE(1) | CTRLDESCL0_3_PITCH(mode->xres * 4),
		(ulong)(priv->reg_base + LCDIFV3_CTRLDESCL0_3));

	lcdifv3_enable_controller(priv);
}

void lcdifv3_power_down(struct lcdifv3_priv *priv)
{
	int timeout = 1000000;

	/* Disable LCDIF during VBLANK */
	writel(INT_STATUS_D0_VS_BLANK,
		(ulong)(priv->reg_base + LCDIFV3_INT_STATUS_D0));
	while (--timeout) {
		if (readl((ulong)(priv->reg_base + LCDIFV3_INT_STATUS_D0)) &
		    INT_STATUS_D0_VS_BLANK)
			break;
		udelay(1);
	}

	lcdifv3_disable_controller(priv);
}

static int lcdifv3_of_get_timings(struct udevice *dev,
			      struct display_timing *timings)
{
	int ret = 0;
	struct lcdifv3_priv *priv = dev_get_priv(dev);

	priv->disp_dev = video_link_get_next_device(dev);
	if (!priv->disp_dev ||
		(device_get_uclass_id(priv->disp_dev) != UCLASS_VIDEO_BRIDGE
		&& device_get_uclass_id(priv->disp_dev) != UCLASS_DISPLAY)) {

		printf("fail to find output device\n");
		return -ENODEV;
	}

	debug("disp_dev %s\n", priv->disp_dev->name);

	ret = video_link_get_display_timings(timings);
	if (ret) {
		printf("fail to get display timings\n");
		return ret;
	}

	return ret;
}

static int lcdifv3_check_thres_value(u32 mul, u32 div)
{
	if (!div)
		return -EINVAL;

	if (mul > div)
		return -EINVAL;

	return 0;
}

static void lcdifv3_of_parse_thres(struct udevice *dev)
{
	int ret;
	u32 thres_low[2], thres_high[2];
	struct lcdifv3_priv *priv = dev_get_priv(dev);


	/* default 'thres-low' value:  FIFO * 1/3;
	 * default 'thres-high' value: FIFO * 2/3.
	 */
	priv->thres_low_mul	= 1;
	priv->thres_low_div	= 3;
	priv->thres_high_mul	= 2;
	priv->thres_high_div	= 3;

	ret = dev_read_u32_array(dev, "thres-low", thres_low, 2);
	if (!ret) {
		/* check the value effectiveness */
		ret = lcdifv3_check_thres_value(thres_low[0], thres_low[1]);
		if (!ret) {
			priv->thres_low_mul	= thres_low[0];
			priv->thres_low_div	= thres_low[1];
		}
	}

	ret = dev_read_u32_array(dev, "thres-high", thres_high, 2);
	if (!ret) {
		/* check the value effectiveness */
		ret = lcdifv3_check_thres_value(thres_high[0], thres_high[1]);
		if (!ret) {
			priv->thres_high_mul	= thres_high[0];
			priv->thres_high_div	= thres_high[1];
		}
	}
}

static int lcdifv3_video_probe(struct udevice *dev)
{
	struct video_uc_plat *plat = dev_get_uclass_plat(dev);
	struct video_priv *uc_priv = dev_get_uclass_priv(dev);
	struct lcdifv3_priv *priv = dev_get_priv(dev);

	struct ctfb_res_modes mode;
	struct display_timing timings;

	u32 fb_start, fb_end;
	int ret;

	debug("%s() plat: base 0x%lx, size 0x%x\n",
	       __func__, plat->base, plat->size);

	priv->reg_base = dev_read_addr(dev);
	if (priv->reg_base == FDT_ADDR_T_NONE) {
		dev_err(dev, "lcdif base address is not found\n");
		return -EINVAL;
	}

	ret = lcdifv3_of_get_timings(dev, &timings);
	if (ret)
		return ret;

	lcdifv3_of_parse_thres(dev);

	if (priv->disp_dev) {
#if IS_ENABLED(CONFIG_VIDEO_BRIDGE)
		if (device_get_uclass_id(priv->disp_dev) == UCLASS_VIDEO_BRIDGE) {
			ret = video_bridge_attach(priv->disp_dev);
			if (ret) {
				dev_err(dev, "fail to attach bridge\n");
				return ret;
			}

			ret = video_bridge_check_timing(priv->disp_dev, &timings);
			if (ret) {
				dev_err(dev, "fail to check timing\n");
				return ret;
			}

			ret = video_bridge_set_backlight(priv->disp_dev, 80);
			if (ret) {
				dev_err(dev, "fail to set backlight\n");
				return ret;
			}
		}
#endif
	}

	mode.xres = timings.hactive.typ;
	mode.yres = timings.vactive.typ;
	mode.left_margin = timings.hback_porch.typ;
	mode.right_margin = timings.hfront_porch.typ;
	mode.upper_margin = timings.vback_porch.typ;
	mode.lower_margin = timings.vfront_porch.typ;
	mode.hsync_len = timings.hsync_len.typ;
	mode.vsync_len = timings.vsync_len.typ;
	mode.pixclock = HZ2PS(timings.pixelclock.typ);
	mode.sync = FB_SYNC_HOR_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT;

	if (timings.flags & DISPLAY_FLAGS_HSYNC_LOW )
		mode.sync &= ~FB_SYNC_HOR_HIGH_ACT;

	if (timings.flags & DISPLAY_FLAGS_VSYNC_LOW )
		mode.sync &= ~FB_SYNC_VERT_HIGH_ACT;

	lcdifv3_init(dev, &mode, VIDEO_BPP32);

	uc_priv->bpix = VIDEO_BPP32; /* only support 32 BPP now */
	uc_priv->xsize = mode.xres;
	uc_priv->ysize = mode.yres;

	/* Enable dcache for the frame buffer */
	fb_start = plat->base & ~(MMU_SECTION_SIZE - 1);
	fb_end = plat->base + plat->size;
	fb_end = ALIGN(fb_end, 1 << MMU_SECTION_SHIFT);
	mmu_set_region_dcache_behaviour(fb_start, fb_end - fb_start,
					DCACHE_WRITEBACK);
	video_set_flush_dcache(dev, true);
	gd->fb_base = plat->base;

	return ret;
}

static int lcdifv3_video_bind(struct udevice *dev)
{
	struct video_uc_plat *plat = dev_get_uclass_plat(dev);

	/* Max size supported by LCDIF, because in bind, we can't probe panel */
	plat->size = 1920 * 1080 *4 * 2;

	return 0;
}

static int lcdifv3_video_remove(struct udevice *dev)
{
	struct lcdifv3_priv *priv = dev_get_priv(dev);

	debug("%s\n", __func__);

	if (priv->disp_dev)
		device_remove(priv->disp_dev, DM_REMOVE_NORMAL);

	lcdifv3_power_down(priv);

	return 0;
}

static const struct udevice_id lcdifv3_video_ids[] = {
	{ .compatible = "fsl,imx8mp-lcdif1" },
	{ .compatible = "fsl,imx93-lcdif" },
	{ /* sentinel */ }
};

U_BOOT_DRIVER(lcdifv3_video) = {
	.name	= "lcdifv3_video",
	.id	= UCLASS_VIDEO,
	.of_match = lcdifv3_video_ids,
	.bind	= lcdifv3_video_bind,
	.probe	= lcdifv3_video_probe,
	.remove = lcdifv3_video_remove,
	.flags	= DM_FLAG_PRE_RELOC | DM_FLAG_OS_PREPARE,
	.priv_auto   = sizeof(struct lcdifv3_priv),
};
