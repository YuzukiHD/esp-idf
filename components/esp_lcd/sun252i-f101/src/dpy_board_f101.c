// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
/*
 * Board description of the display pipeline of the F101 EVB:
 *
 *   display engine -> TCON LCD -> RGB encoder -> 1024x600 RGB666 panel
 *                                                   (+ PWM_BL backlight)
 *
 * The node names, resources, links and platform data are the ones of the
 * pipeline graph the display core consumes (dpy_graph.h).
 */
#include <dpy/dpy_graph.h>
#include <dpy/dpy_os.h>
#include <dpy/dpy_pdata.h>
#include <dpy/dpy_tcon.h>
#include <sunxi_ccu/sun252i-f101-ccu.h>

#include "dt_graph.h"

#define DE_NAME		"display-engine@5000000"
#define TOP_NAME	"tcon-top@5460000"
#define LCD_NAME	"tcon-lcd@5461000"
#define RGB_NAME	"rgb"
#define PANEL_NAME	"panel"
#define BL_NAME		"backlight"

#define PIN(bank, n)	DPY_SUNXI_PIN(bank, n)

/* ------------------------------------------------------------------ */
/* Resources                                                           */
/* ------------------------------------------------------------------ */
static const struct dpy_res de_res[] = {
	DPY_RES_MMIO("reg", 0x05000000, 0x200000),
	DPY_RES_CLK("mod", ALLWINNER_CCU_MAIN, CLK_DE, CLK_PLL_PERI_2X, 300000000),
	DPY_RES_CLK("bus", ALLWINNER_CCU_MAIN, CLK_BUS_DE, DPY_CLK_NO_PARENT, 0),
	DPY_RES_RST("bus", ALLWINNER_CCU_MAIN, RST_BUS_DE),
};

static const struct dpy_res top_res[] = {
	DPY_RES_MMIO("reg", 0x05460000, 0x1000),
	DPY_RES_CLK("bus", ALLWINNER_CCU_MAIN, CLK_BUS_DPSS_TOP, DPY_CLK_NO_PARENT, 0),
	DPY_RES_RST("bus", ALLWINNER_CCU_MAIN, RST_BUS_DPSS_TOP),
};

static const struct dpy_res lcd_res[] = {
	DPY_RES_MMIO("reg", 0x05461000, 0x1000),
	DPY_RES_IRQ("irq", 90),
	DPY_RES_CLK("mod", ALLWINNER_CCU_MAIN, CLK_TCONLCD, CLK_PLL_VIDEO0_4X, 0),
	DPY_RES_CLK("bus", ALLWINNER_CCU_MAIN, CLK_BUS_TCONLCD, DPY_CLK_NO_PARENT, 0),
	DPY_RES_RST("bus", ALLWINNER_CCU_MAIN, RST_BUS_TCONLCD),
};

static const struct dpy_ref lcd_refs[] = {
	{ "top", TOP_NAME },
};

static const struct dpy_ref panel_refs[] = {
	{ "backlight", BL_NAME },
};

/* ------------------------------------------------------------------ */
/* Platform data                                                       */
/* ------------------------------------------------------------------ */
static const struct dpy_engine_pdata de_pdata = {
	.adjust = { .brightness = 50, .contrast = 50, .saturation = 57, .hue = 50 },
	.background = 0,
};

/* PD0..PD9, PD12..PD21 at function 2, PE6/PE7 (clock, data enable) at function 4 */
static const struct dpy_pin rgb_pins[] = {
	DPY_PIN(PIN('D', 0), 2),  DPY_PIN(PIN('D', 1), 2),  DPY_PIN(PIN('D', 2), 2),  DPY_PIN(PIN('D', 3), 2),
	DPY_PIN(PIN('D', 4), 2),  DPY_PIN(PIN('D', 5), 2),  DPY_PIN(PIN('D', 6), 2),  DPY_PIN(PIN('D', 7), 2),
	DPY_PIN(PIN('D', 8), 2),  DPY_PIN(PIN('D', 9), 2),
	DPY_PIN(PIN('E', 6), 4),  DPY_PIN(PIN('E', 7), 4),
	DPY_PIN(PIN('D', 12), 2), DPY_PIN(PIN('D', 13), 2), DPY_PIN(PIN('D', 14), 2), DPY_PIN(PIN('D', 15), 2),
	DPY_PIN(PIN('D', 16), 2), DPY_PIN(PIN('D', 17), 2), DPY_PIN(PIN('D', 18), 2), DPY_PIN(PIN('D', 19), 2),
	DPY_PIN(PIN('D', 20), 2), DPY_PIN(PIN('D', 21), 2),
};

static const struct dpy_rgb_pdata rgb_pdata = {
	.pins = DPY_PIN_GROUP(rgb_pins),
	.hv_mode = DPY_HV_PARALLEL_RGB,
	.rgb_swap = 5,
};

/* 49 MHz, 1024 + 161 + 20 + 35 by 600 + 15 + 10 + 25, both syncs active low */
static const struct dpy_display_mode panel_modes[] = {
	{
		.clock = 49000,
		.hdisplay = 1024, .hsync_start = 1185, .hsync_end = 1205, .htotal = 1240,
		.vdisplay = 600, .vsync_start = 615, .vsync_end = 625, .vtotal = 650,
		.flags = DISPLAY_MODE_FLAG_NHSYNC | DISPLAY_MODE_FLAG_NVSYNC | DISPLAY_MODE_FLAG_PREFERRED,
	},
};

/* PD22 switches the panel supply */
static const struct dpy_cmd panel_on_cmds[] = {
	DPY_CMD_SET_GPIO(PIN('D', 22), 1),
	DPY_CMD_DELAY_MS(30),
};

static const struct dpy_cmd panel_off_cmds[] = {
	DPY_CMD_SET_GPIO(PIN('D', 22), 0),
	DPY_CMD_DELAY_MS(20),
};

static const struct dpy_panel_simple_pdata panel_pdata = {
	.name = PANEL_NAME,
	.modes = panel_modes,
	.num_modes = DPY_ARRAY_SIZE(panel_modes),
	.width_mm = 154,
	.height_mm = 86,
	.bus_format = DPY_BUS_FMT_RGB666_1X18,
	.bus_flags = 0,
	.power_on = DPY_CMD_SEQ(panel_on_cmds),
	.power_off = DPY_CMD_SEQ(panel_off_cmds),
	.enable_delay_ms = 50,
};

/* PB0..PB3 carry the PWM_BL outputs at mux 2 */
static const struct dpy_pin bl_pins[] = {
	DPY_PIN(PIN('B', 0), 2), DPY_PIN(PIN('B', 1), 2), DPY_PIN(PIN('B', 2), 2), DPY_PIN(PIN('B', 3), 2),
};

static const struct dpy_backlight_pwm_pdata bl_pdata = {
	/* the one PWM_BL block is addressed by dpy_os_pwm_apply() itself, the handle only has to be set */
	.controller = &bl_pins,
	.channel = 0,
	.pins = DPY_PIN_GROUP(bl_pins),
	.period_ns = 20000,
	.inverted = true,
	.max_level = 255,
	.default_level = 250,
	.min_level = 0,
};

/* ------------------------------------------------------------------ */
/* Graph                                                               */
/* ------------------------------------------------------------------ */
static const struct dpy_node nodes[] = {
	{ .name = DE_NAME, .compatible = DPY_COMPATIBLE("allwinner,sun252iw2-display-engine"),
	  .status = DPY_STATUS_OKAY, .res = de_res, .nres = DPY_ARRAY_SIZE(de_res), .pdata = &de_pdata },
	{ .name = TOP_NAME, .compatible = DPY_COMPATIBLE("allwinner,sun252iw2-tcon-top", "allwinner,sunxi-tcon-top"),
	  .status = DPY_STATUS_OKAY, .res = top_res, .nres = DPY_ARRAY_SIZE(top_res) },
	{ .name = LCD_NAME, .compatible = DPY_COMPATIBLE("allwinner,sun252iw2-tcon-lcd", "allwinner,sunxi-tcon-lcd"),
	  .status = DPY_STATUS_OKAY, .res = lcd_res, .nres = DPY_ARRAY_SIZE(lcd_res),
	  .refs = lcd_refs, .nrefs = DPY_ARRAY_SIZE(lcd_refs) },
	{ .name = RGB_NAME, .compatible = DPY_COMPATIBLE("allwinner,sunxi-rgb"),
	  .status = DPY_STATUS_OKAY, .pdata = &rgb_pdata },
	{ .name = PANEL_NAME, .compatible = DPY_COMPATIBLE("panel-simple"),
	  .status = DPY_STATUS_OKAY, .refs = panel_refs, .nrefs = DPY_ARRAY_SIZE(panel_refs), .pdata = &panel_pdata },
	{ .name = BL_NAME, .compatible = DPY_COMPATIBLE("pwm-backlight"),
	  .status = DPY_STATUS_OKAY, .pdata = &bl_pdata },
};

static const struct dpy_link links[] = {
	{ DE_NAME, 0, 0, LCD_NAME, 0, 0 },
	{ LCD_NAME, 1, 0, RGB_NAME, 0, 0 },
	{ RGB_NAME, 1, 0, PANEL_NAME, 0, 0 },
};

const struct dpy_board_desc dpy_board = {
	.name = "f101-evb",
	.nodes = nodes,
	.nnodes = DPY_ARRAY_SIZE(nodes),
	.links = links,
	.nlinks = DPY_ARRAY_SIZE(links),
};
