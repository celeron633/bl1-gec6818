/*
 * Boot banner on the GEC6818's RGB LCD (AT070TN92, 800x480).
 *
 * A trimmed copy of what u-boot does in nx_rgb_display()
 * (drivers/video/nexell/s5pxx18_dp.c, s5pxx18_dp_rgb.c) with the values
 * u-boot ends up with for this board: DPC0 -> MLC0, RGB layer 1,
 * XRGB8888, pixel clock from PLL3 (800MHz) / 26, RGB666 out with 6-bit
 * dither. Register accesses follow u-boot's read-modify-write order so
 * the result matches it bit for bit.
 *
 * To keep DDR traffic tiny (BL1 runs with the D-cache off) the RGB layer
 * only covers the text; the rest of the screen is the MLC background
 * color, which costs no memory at all. u-boot resets the display blocks
 * and sets them up again for its own logo later.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

#include "sysheader.h"
#include "font8x16.h"

extern void ResetCon(U32 devicenum, CBOOL en);

/* panel: u-boot board/s5p6818/gec6818/lcds.c "at070tn92" */
#define LCD_WIDTH	800
#define LCD_HEIGHT	480
#define LCD_HFP		80
#define LCD_HBP		36
#define LCD_HSW		10
#define LCD_VFP		22
#define LCD_VBP		15
#define LCD_VSW		8
#define LCD_FREQ	61
/* u-boot's nx_display_fixup_dp(): 800MHz / (926 * 525 * 61) = 26 */
#define LCD_PLL_HZ	800000000
#define LCD_CLK_DIV	(LCD_PLL_HZ / ((LCD_HSW + LCD_HBP + LCD_HFP + LCD_WIDTH) * \
			(LCD_VSW + LCD_VBP + LCD_VFP + LCD_HEIGHT) * LCD_FREQ))

/* framebuffer: u-boot's CONFIG_FB_ADDR, 4 bytes per pixel */
#define FB_BASE		0x46000000
#define BG_COLOR	0x000000
#define FG_COLOR	0xFFFFFF
#define TEXT_SCALE	2
#define GLYPH_W		(FONT_WIDTH * TEXT_SCALE)
#define GLYPH_H		(FONT_HEIGHT * TEXT_SCALE)

#define DISPTOP_TFTMPU_MUX	0xC0101024
#define MLC0			0xC0102000
#define DPC0			0xC0102800

/* MLC0 */
#define MLCCONTROLT	(MLC0 + 0x000)
#define MLCSCREENSIZE	(MLC0 + 0x004)
#define MLCBGCOLOR	(MLC0 + 0x008)
#define MLCRGB1(off)	(MLC0 + 0x040 + (off))	/* RGB layer 1 */
#define RGB_LEFTRIGHT	0x00
#define RGB_TOPBOTTOM	0x04
#define RGB_INVALID0_LR	0x08	/* .. four words up to 0x14 */
#define RGB_CONTROL	0x18
#define RGB_HSTRIDE	0x1C
#define RGB_VSTRIDE	0x20
#define RGB_TPCOLOR	0x24
#define RGB_INVCOLOR	0x28
#define RGB_ADDRESS	0x2C
#define MLCGAMMACONT	(MLC0 + 0x0F0)
#define MLCCLKENB	(MLC0 + 0x3C0)

#define MLC_TOP_DIRTY	(1 << 3)
#define MLC_RGB_DIRTY	(1 << 4)
#define MLC_FMT_XRGB8888 0x06530000

/* DPC0 */
#define DPCHTOTAL	(DPC0 + 0x0F8)	/* HSWIDTH, HASTART, HAEND follow */
#define DPCVTOTAL	(DPC0 + 0x108)	/* VSWIDTH, VASTART, VAEND follow */
#define DPCCTRL0	(DPC0 + 0x118)
#define DPCCTRL1	(DPC0 + 0x11C)
#define DPCEVTOTAL	(DPC0 + 0x120)	/* EVSWIDTH, EVASTART, EVAEND follow */
#define DPCCTRL2	(DPC0 + 0x130)
#define DPCVSEOFFSET	(DPC0 + 0x134)	/* VSSOFFSET, EVSEOFFSET, EVSSOFFSET */
#define DPCDELAY0	(DPC0 + 0x144)
#define DPCDELAY1	(DPC0 + 0x178)
#define DPCCLKENB	(DPC0 + 0x3C0)
#define DPCCLKGEN(i, j)	(DPC0 + 0x3C4 + (i) * 8 + (j) * 4)

#define DPC_INTPEND	(1 << 10)

/* GPIO A (RGB pads) and D (backlight, PWM0 pad) */
#define GPIOA		0xC001A000
#define GPIOD		0xC001D000
#define GPIO_OUT	0x00
#define GPIO_OUTENB	0x04
#define GPIO_ALTFN0	0x20
#define GPIO_ALTFN1	0x24
#define GPIO_DRV1	0x48
#define GPIO_DRV0	0x50
#define GPIO_PULLSEL	0x58
#define GPIO_PULLSEL_DD	0x5C
#define GPIO_PULLENB	0x60
#define GPIO_PULLENB_DD	0x64

static void rmw(MPTRS addr, U32 clear, U32 set)
{
	WriteIO32(addr, (ReadIO32(addr) & ~clear) | set);
}

static void lcd_gpio_init(void)
{
	const U32 pins = 0x0FFFFFFF; /* GPIOA 0..27: PVCLK, RGB24, HS, VS, DE */

	/* board.c bd_lcd_config_gpio(): alt function 1, pull off */
	WriteIO32(GPIOA + GPIO_ALTFN0, 0x55555555);
	rmw(GPIOA + GPIO_ALTFN1, 0x00FFFFFF, 0x00555555);
	rmw(GPIOA + GPIO_PULLSEL_DD, 0, pins);
	rmw(GPIOA + GPIO_PULLENB_DD, 0, pins);
	rmw(GPIOA + GPIO_PULLENB, pins, 0);
	rmw(GPIOA + GPIO_PULLSEL, pins, 0);
	/*
	 * lcds.c s70_gpio_init() (at070tn92): PVCLK 1, RGB24 2, HS/VS/DE 1.
	 * DRV1 holds bit 0 of each pin's strength, DRV0 bit 1.
	 */
	rmw(GPIOA + GPIO_DRV1, pins, 0x0E000001);
	rmw(GPIOA + GPIO_DRV0, pins, 0x01FFFFFE);

#if defined(LCD_BACKLIGHT_LEVEL)
	/*
	 * GPIOD1 is PWM0, the backlight control u-boot drives at 50% duty.
	 * BL1 has no PWM clock set up, so just hold the pad at a fixed level.
	 */
	rmw(GPIOD + GPIO_ALTFN0, 3 << 2, 0);
	rmw(GPIOD + GPIO_OUT, 1 << 1, LCD_BACKLIGHT_LEVEL << 1);
	rmw(GPIOD + GPIO_OUTENB, 0, 1 << 1);
#endif
}

static void draw_text(U32 *fb, U32 stride, const char *s)
{
	int x, y;

	for (; *s; s++, fb += GLYPH_W) {
		unsigned char c = *s;
		const unsigned char *glyph;

		if (c < FONT_FIRST || c > FONT_LAST)
			c = '?';
		glyph = font8x16[c - FONT_FIRST];
		for (y = 0; y < GLYPH_H; y++) {
			U32 bits = glyph[y / TEXT_SCALE];
			U32 *p = fb + y * stride;

			for (x = 0; x < GLYPH_W; x++)
				p[x] = (bits & (0x80 >> (x / TEXT_SCALE))) ?
					FG_COLOR : BG_COLOR;
		}
	}
}

void DisplayBanner(const char *text)
{
	U32 len = 0, w, h = GLYPH_H, sx, sy;
	int i;

	while (text[len])
		len++;
	w = len * GLYPH_W;
	if (w == 0 || w > LCD_WIDTH)
		return;
	sx = (LCD_WIDTH - w) / 2;
	sy = (LCD_HEIGHT - h) / 2;

	draw_text((U32 *)(MPTRS)FB_BASE, w, text);
	lcd_gpio_init();

	/* dp_control_init() */
	ResetCon(RESETINDEX_OF_DISPLAYTOP_MODULE_i_Top_nRST, CTRUE);
	ResetCon(RESETINDEX_OF_DISPLAYTOP_MODULE_i_Top_nRST, CFALSE);
	ResetCon(RESETINDEX_OF_DISPLAYTOP_MODULE_i_DualDisplay_nRST, CTRUE);
	ResetCon(RESETINDEX_OF_DISPLAYTOP_MODULE_i_DualDisplay_nRST, CFALSE);
	rmw(DPCCLKENB, 1 << 3, 1 << 3);		/* PCLK always */

	/* dp_plane_init(): PCLK always, BCLK always */
	rmw(MLCCLKENB, (1 << 3) | 3, (1 << 3) | 3);

	/* dp_plane_screen_setup() */
	WriteIO32(MLCSCREENSIZE, ((LCD_HEIGHT - 1) << 16) | (LCD_WIDTH - 1));
	rmw(MLCCONTROLT, (3 << 8) | MLC_TOP_DIRTY | 1, 0); /* prio 0, no field */
	WriteIO32(MLCBGCOLOR, BG_COLOR);
	/* gamma table power/sleep bits, gamma, dither and alpha select: 0 */
	rmw(MLCGAMMACONT, 0xF2F, 0);
	/* pixel buffer powered, not sleeping */
	rmw(MLCCONTROLT, MLC_TOP_DIRTY, (1 << 11) | (1 << 10));

	/* dp_plane_layer_setup(): lock size 16, no blend/tp/inversion */
	rmw(MLCRGB1(RGB_CONTROL), 0xFFFF0000 | (3 << 12) | MLC_RGB_DIRTY | 7,
	    MLC_FMT_XRGB8888 | (2 << 12));
	rmw(MLCRGB1(RGB_TPCOLOR), 0xF0FFFFFF, 15u << 28);
	rmw(MLCRGB1(RGB_INVCOLOR), 0x00FFFFFF, 0);
	for (i = 0; i < 4; i++)
		WriteIO32((MPTRS)MLCRGB1(RGB_INVALID0_LR) + i * 4, 0);
	WriteIO32(MLCRGB1(RGB_LEFTRIGHT), (sx << 16) | (sx + w - 1));
	WriteIO32(MLCRGB1(RGB_TOPBOTTOM), (sy << 16) | (sy + h - 1));
	WriteIO32(MLCRGB1(RGB_HSTRIDE), 4);
	WriteIO32(MLCRGB1(RGB_VSTRIDE), w * 4);
	WriteIO32(MLCRGB1(RGB_ADDRESS), FB_BASE);

	/* dp_plane_set_enable() */
	rmw(MLCRGB1(RGB_CONTROL), MLC_RGB_DIRTY, 1 << 5);
	rmw(MLCRGB1(RGB_CONTROL), 0, MLC_RGB_DIRTY);

	/* dp_plane_screen_enable() */
	rmw(MLCCONTROLT, MLC_TOP_DIRTY, 1 << 1);
	rmw(MLCCONTROLT, 0, MLC_TOP_DIRTY);

	/* rgb_switch(): primary MLC -> RGB pads */
	WriteIO32(DISPTOP_TFTMPU_MUX, 0);

	/*
	 * dp_control_setup(). CLKGEN0: source "3" (PLL3), which u-boot
	 * remaps to select value 6; CLKGEN1: source 7, divide by 1.
	 */
	rmw(DPCCLKGEN(0, 0), (7 << 2) | (0xFF << 5), (6 << 2) | ((LCD_CLK_DIV - 1) << 5));
	rmw(DPCCLKGEN(1, 0), (7 << 2) | (0xFF << 5), 7 << 2);
	rmw(DPCCLKGEN(0, 1), 0x1F, 0);
	rmw(DPCCLKGEN(1, 1), 0x1F, 0);

	/* nx_dpc_set_mode(): RGB666, progressive, RGB mode, VCLK pad clock */
	rmw(DPCCTRL0, DPC_INTPEND | (1 << 9) | (1 << 8) | (1 << 2), 1 << 12);
	WriteIO32(DPCCTRL1, (ReadIO32(DPCCTRL1) & 0x3F) | (2 << 8) | (1 << 13));
	rmw(DPCCTRL2, 1 << 4, 0);
	rmw(DPCCTRL2, (3 << 0) | (3 << 7), 0);
	rmw(DPCCLKGEN(0, 0), 1 << 1, 0);
	rmw(DPCCLKGEN(1, 0), 1 << 1, 0);

	/* nx_dpc_set_hsync()/vsync(): active-low syncs (u-boot inverts inv_*) */
	WriteIO32(DPCHTOTAL + 0x0, LCD_HSW + LCD_HBP + LCD_WIDTH + LCD_HFP - 1);
	WriteIO32(DPCHTOTAL + 0x4, LCD_HSW - 1);
	WriteIO32(DPCHTOTAL + 0x8, LCD_HSW + LCD_HBP - 1);
	WriteIO32(DPCHTOTAL + 0xC, LCD_HSW + LCD_HBP + LCD_WIDTH - 1);
	rmw(DPCCTRL0, DPC_INTPEND | (1 << 0), 0);
	for (i = 0; i < 2; i++) {
		MPTRS base = i ? DPCEVTOTAL : DPCVTOTAL;

		WriteIO32(base + 0x0, LCD_VSW + LCD_VBP + LCD_HEIGHT + LCD_VFP - 1);
		WriteIO32(base + 0x4, LCD_VSW - 1);
		WriteIO32(base + 0x8, LCD_VSW + LCD_VBP - 1);
		WriteIO32(base + 0xC, LCD_VSW + LCD_VBP + LCD_HEIGHT - 1);
	}
	rmw(DPCCTRL0, DPC_INTPEND | (1 << 1), 0);

	/* nx_dpc_set_vsync_offset(1, 1, 1, 1) */
	for (i = 0; i < 4; i++)
		WriteIO32((MPTRS)DPCVSEOFFSET + i * 4, 1);

	/* nx_dpc_set_delay(0, 7, 7, 7) */
	rmw(DPCCTRL0, DPC_INTPEND | (0xF << 4), 0);
	WriteIO32(DPCDELAY0, (7 << 8) | 7);
	WriteIO32(DPCDELAY1, 7);

	/* nx_dpc_set_dither(): 6 bit for R, G and B */
	rmw(DPCCTRL1, 0x3F, 0x3F);

	/* dp_control_enable() */
	rmw(DPCCTRL0, DPC_INTPEND | (1 << 15), 1 << 15);
	rmw(DPCCLKENB, 1 << 2, 1 << 2);

	SYSMSG("LCD: %dx%d, \"%s\" at %d,%d, fb 0x%08X\r\n",
	       LCD_WIDTH, LCD_HEIGHT, text, sx, sy, FB_BASE);
}
