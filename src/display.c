/*
 * Boot progress on the GEC6818's RGB LCD (AT070TN92, 800x480):
 * DisplayInit() puts up a centered title and an empty progress bar at the
 * bottom; DisplayStep() adds a line below the title and moves the bar to
 * the given percentage, animated over the BOOT_LOGO_DELAY_MS it then waits
 * so each step can be read.
 *
 * A trimmed copy of what u-boot does in nx_rgb_display()
 * (drivers/video/nexell/s5pxx18_dp.c, s5pxx18_dp_rgb.c) with the values
 * u-boot ends up with for this board: DPC0 -> MLC0, RGB layer 1,
 * XRGB8888, pixel clock from PLL3 (800MHz) / 26, RGB666 out with 6-bit
 * dither. Register accesses follow u-boot's read-modify-write order so
 * the result matches it bit for bit.
 *
 * To keep DDR traffic small (BL1 runs with the D-cache off) RGB layer 1
 * only covers the lines printed so far and grows by one line per
 * DisplayStep(), and RGB layer 0 only covers the progress bar; the rest of
 * the screen is the MLC background color, which costs no memory at all.
 * u-boot resets the display blocks and sets them up again for its own
 * logo later.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

#include <stdarg.h>
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

/* framebuffers, 4 bytes per pixel: text at u-boot's CONFIG_FB_ADDR, bar after */
#define FB_BASE		0x46000000
#define FB_BAR		(FB_BASE + LCD_WIDTH * LCD_HEIGHT * 4)
#define BG_COLOR	0x000000
#define FG_COLOR	0xFFFFFF
#define TEXT_SCALE	2
#define GLYPH_W		(FONT_WIDTH * TEXT_SCALE)
#define GLYPH_H		(FONT_HEIGHT * TEXT_SCALE)
#define TEXT_COLS	(LCD_WIDTH / GLYPH_W)	/* 50 */
#define TEXT_ROWS	12
#define TEXT_TOP	16

/* progress bar: white outline, a black gap, then the fill */
#define BAR_COLOR	0x3399FF
#define BAR_X		40
#define BAR_Y		(TEXT_TOP + TEXT_ROWS * GLYPH_H + 24)
#define BAR_W		(LCD_WIDTH - 2 * BAR_X)
#define BAR_H		24
#define BAR_BORDER	2
#define BAR_INSET	(2 * BAR_BORDER)
#define BAR_FILL_W	(BAR_W - 2 * BAR_INSET)

/*
 * TIMER ch0 as a 1MHz down-counter for the delays, set up like u-boot's
 * timer_init(): PCLK (200MHz, clockinit.c: PLL3 / 2 / 2) / prescaler 200.
 */
#define TIMER		0xC0017000
#define TIMER_TCFG0	(TIMER + 0x00)
#define TIMER_TCFG1	(TIMER + 0x04)
#define TIMER_TCON	(TIMER + 0x08)
#define TIMER_TCNTB0	(TIMER + 0x0C)
#define TIMER_TCMPB0	(TIMER + 0x10)
#define TIMER_TCNTO0	(TIMER + 0x14)
#define TIMER_PCLK_HZ	200000000

#define DISPTOP_TFTMPU_MUX	0xC0101024
#define MLC0			0xC0102000
#define DPC0			0xC0102800

/* MLC0 */
#define MLCCONTROLT	(MLC0 + 0x000)
#define MLCSCREENSIZE	(MLC0 + 0x004)
#define MLCBGCOLOR	(MLC0 + 0x008)
#define MLCRGB(l, off)	((MPTRS)MLC0 + 0x00C + (l) * 0x34 + (off))  /* RGB layer l */
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

extern U32 getquotient(U32 dividend, U32 divisor);

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

/* one full-width text row, padded with spaces, centered or indented by one */
static void draw_row(U32 row, const char *s, int center)
{
	U32 *fb = (U32 *)(MPTRS)FB_BASE + row * GLYPH_H * LCD_WIDTH;
	U32 len = 0, pad, col, x, y;

	while (s[len] && len < TEXT_COLS - 2)
		len++;
	pad = center ? (TEXT_COLS - len) / 2 : 1;
	for (col = 0; col < TEXT_COLS; col++, fb += GLYPH_W) {
		unsigned char c = (col >= pad && col - pad < len) ? s[col - pad] : ' ';
		const unsigned char *glyph;

		if (c < FONT_FIRST || c > FONT_LAST)
			c = '?';
		glyph = font8x16[c - FONT_FIRST];
		for (y = 0; y < GLYPH_H; y++) {
			U32 bits = glyph[y / TEXT_SCALE];
			U32 *p = fb + y * LCD_WIDTH;

			for (x = 0; x < GLYPH_W; x++)
				p[x] = (bits & (0x80 >> (x / TEXT_SCALE))) ?
					FG_COLOR : BG_COLOR;
		}
	}
}

/* make the text layer cover rows 0..rows-1, from the next frame on */
static void show_rows(U32 rows)
{
	WriteIO32(MLCRGB(1, RGB_TOPBOTTOM),
		  (TEXT_TOP << 16) | (TEXT_TOP + rows * GLYPH_H - 1));
	rmw(MLCRGB(1, RGB_CONTROL), 0, MLC_RGB_DIRTY);
}

/* rectangle in the bar's framebuffer */
static void bar_rect(U32 x0, U32 y0, U32 x1, U32 y1, U32 color)
{
	U32 *fb = (U32 *)(MPTRS)FB_BAR, x, y;

	for (y = y0; y < y1; y++)
		for (x = x0; x < x1; x++)
			fb[y * BAR_W + x] = color;
}

static void bar_init(void)
{
	bar_rect(0, 0, BAR_W, BAR_H, FG_COLOR);
	bar_rect(BAR_BORDER, BAR_BORDER, BAR_W - BAR_BORDER, BAR_H - BAR_BORDER,
		 BG_COLOR);
}

/* the bar's fill is bar_shown pixels wide and heading for bar_target */
static U32 bar_shown, bar_target;

static void bar_grow(U32 to)
{
	if (to <= bar_shown)
		return;
	bar_rect(BAR_INSET + bar_shown, BAR_INSET, BAR_INSET + to,
		 BAR_H - BAR_INSET, BAR_COLOR);
	bar_shown = to;
}

/* dp_plane_layer_setup() + dp_plane_set_enable() for RGB layer l */
static void layer_setup(U32 l, U32 x, U32 y, U32 w, U32 h, U32 fb)
{
	int i;

	/* lock size 16, no blend/tp/inversion */
	rmw(MLCRGB(l, RGB_CONTROL), 0xFFFF0000 | (3 << 12) | MLC_RGB_DIRTY | 7,
	    MLC_FMT_XRGB8888 | (2 << 12));
	rmw(MLCRGB(l, RGB_TPCOLOR), 0xF0FFFFFF, 15u << 28);
	rmw(MLCRGB(l, RGB_INVCOLOR), 0x00FFFFFF, 0);
	for (i = 0; i < 4; i++)
		WriteIO32(MLCRGB(l, RGB_INVALID0_LR) + i * 4, 0);
	WriteIO32(MLCRGB(l, RGB_LEFTRIGHT), (x << 16) | (x + w - 1));
	WriteIO32(MLCRGB(l, RGB_TOPBOTTOM), (y << 16) | (y + h - 1));
	WriteIO32(MLCRGB(l, RGB_HSTRIDE), 4);
	WriteIO32(MLCRGB(l, RGB_VSTRIDE), w * 4);
	WriteIO32(MLCRGB(l, RGB_ADDRESS), fb);

	rmw(MLCRGB(l, RGB_CONTROL), MLC_RGB_DIRTY, 1 << 5);
	rmw(MLCRGB(l, RGB_CONTROL), 0, MLC_RGB_DIRTY);
}

static void timer_init(void)
{
	rmw(TIMER_TCFG0, 0xFF, TIMER_PCLK_HZ / 1000000 - 1);
	rmw(TIMER_TCFG1, 0xF, 0);
	WriteIO32(TIMER_TCNTB0, 0xFFFFFFFF);
	WriteIO32(TIMER_TCMPB0, 0xFFFFFFFF);
	rmw(TIMER_TCON, 0xF, 1 << 1);		/* manual update */
	rmw(TIMER_TCON, 0xF, (1 << 3) | 1);	/* auto reload, start */
}

/* wait ms milliseconds, moving the bar's fill to bar_target meanwhile */
static void delay_ms(U32 ms)
{
	U32 start = ReadIO32(TIMER_TCNTO0), from = bar_shown, t;

	do {
		t = getquotient(start - ReadIO32(TIMER_TCNTO0), 1000);
		if (t > ms)
			t = ms;
		bar_grow(t == ms ? bar_target :
			 from + getquotient((bar_target - from) * t, ms));
	} while (t < ms);
}

/* next free text row; -1 until DisplayInit(), so DisplayStep() is a no-op */
static int next_row = -1;

void DisplayInit(const char *title)
{
	int i;

	draw_row(0, title, 1);
	draw_row(1, "", 0);
	bar_shown = bar_target = 0;	/* .bss isn't cleared on every boot path */
	bar_init();
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

	/* text in RGB layer 1 (title + blank row for now), bar in layer 0 */
	layer_setup(1, 0, TEXT_TOP, LCD_WIDTH, 2 * GLYPH_H, FB_BASE);
	layer_setup(0, BAR_X, BAR_Y, BAR_W, BAR_H, FB_BAR);

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

	SYSMSG("LCD: %dx%d, fb 0x%08X, %d ms per line\r\n",
	       LCD_WIDTH, LCD_HEIGHT, FB_BASE, BOOT_LOGO_DELAY_MS);

	timer_init();
	next_row = 2;
	delay_ms(BOOT_LOGO_DELAY_MS);
}

/*
 * printf() a line below the title (once the screen is full, into the last
 * row) and move the progress bar up to percent, never back.
 */
void DisplayStep(U32 percent, const char *fmt, ...)
{
	char buf[TEXT_COLS * 2];
	va_list args;
	U32 target;

	if (next_row < 0)
		return;
	target = getquotient((percent > 100 ? 100 : percent) * BAR_FILL_W, 100);
	if (target > bar_target)
		bar_target = target;
	va_start(args, fmt);
	sprint_va(buf, fmt, &args);
	va_end(args);

	if (next_row == TEXT_ROWS)
		next_row--;
	draw_row(next_row++, buf, 0);
	show_rows(next_row);
	delay_ms(BOOT_LOGO_DELAY_MS);
}
