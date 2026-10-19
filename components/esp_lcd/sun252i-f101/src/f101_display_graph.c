// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
/*
 * The pipeline graph of the display stack, built at run time from what the panel front end was
 * configured with:
 *
 *   RGB: display engine -> TCON LCD -> RGB encoder -> panel
 *   DSI: display engine -> TCON LCD -> MIPI DSI (+ combo D-PHY) -> panel
 *
 * The blocks of the SoC (addresses, clocks, resets, interrupts) are constants; the pads, the timing,
 * the reset line and the panel commands come from the descriptor.
 */
#include <stdlib.h>
#include <string.h>
#include <dpy/dpy_graph.h>
#include <dpy/dpy_os.h>
#include <dpy/dpy_pdata.h>
#include <dpy/dpy_tcon.h>
#include <dpy/dpy_dsi.h>
#include <sunxi_ccu/sun252i-f101-ccu.h>

#include "dt_graph.h"
#include "f101_display_desc.h"

#define DE_NAME     "display-engine@5000000"
#define TOP_NAME    "tcon-top@5460000"
#define LCD_NAME    "tcon-lcd@5461000"
#define RGB_NAME    "rgb"
#define DSI_NAME    "dsi@5450000"
#define DPHY_NAME   "phy@5451000"
#define PANEL_NAME  "panel"

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

static const struct dpy_res dsi_res[] = {
	DPY_RES_MMIO("reg", 0x05450000, 0x1000),
	DPY_RES_IRQ("irq", 92),
	DPY_RES_CLK("mod", ALLWINNER_CCU_MAIN, CLK_DSI, CLK_PLL_PERI_1X, 150000000),
	DPY_RES_CLK("bus", ALLWINNER_CCU_MAIN, CLK_BUS_DSI, DPY_CLK_NO_PARENT, 0),
	DPY_RES_RST("bus", ALLWINNER_CCU_MAIN, RST_BUS_DSI),
};

static const struct dpy_res dphy_res[] = {
	DPY_RES_MMIO("reg", 0x05451000, 0x1000),
	DPY_RES_CLK("mod", ALLWINNER_CCU_MAIN, CLK_COMBOPHY0, CLK_PLL_VIDEO0_4X, 0),
	DPY_RES_CLK("bus", ALLWINNER_CCU_MAIN, CLK_BUS_COMBOPHY0, DPY_CLK_NO_PARENT, 0),
};

static const struct dpy_ref lcd_refs[] = { { "top", TOP_NAME } };
static const struct dpy_ref dsi_refs[] = { { "phy", DPHY_NAME } };

static const struct dpy_engine_pdata de_pdata = {
	.adjust = { .brightness = 50, .contrast = 50, .saturation = 57, .hue = 50 },
	.background = 0,
};

static const char *const c_compat_0[] = { "allwinner,sun252iw2-display-engine", NULL };
static const char *const c_compat_1[] = { "allwinner,sun252iw2-tcon-top", "allwinner,sunxi-tcon-top", NULL };
static const char *const c_compat_2[] = { "allwinner,sun252iw2-tcon-lcd", "allwinner,sunxi-tcon-lcd", NULL };
static const char *const c_compat_3[] = { "allwinner,sunxi-rgb", NULL };
static const char *const c_compat_4[] = { "panel-simple", NULL };
static const char *const c_compat_5[] = { "allwinner,sun252iw2-mipi-dsi", "allwinner,sunxi-mipi-dsi", NULL };
static const char *const c_compat_6[] = { "allwinner,sun252iw2-combo-dphy", "allwinner,sunxi-combo-dphy", NULL };
static const char *const c_compat_7[] = { "panel-dsi", NULL };

/* the descriptor and everything built from it */
static struct {
	f101_display_desc_t desc;
	struct dpy_rgb_pdata rgb_pdata;
	struct dpy_dsi_pdata dsi_pdata;
	struct dpy_panel_simple_pdata simple;
	struct dpy_panel_dsi_pdata dsi_panel;
	struct dpy_pin dsi_pins[10];
	struct dpy_cmd power_on[6], power_off[2];
	struct dpy_cmd *init, *exit;
	uint8_t **bufs;
	size_t nbufs;
	struct dpy_node nodes[8];
	struct dpy_link links[3];
	struct dpy_board_desc board;
	bool valid;
} g;

static struct dpy_cmd *dcs_table(const esp_lcd_dpi_f101_cmd_t *c, size_t n)
{
	struct dpy_cmd *t;

	if (!n)
		return NULL;
	t = calloc(n + 1, sizeof(*t));
	if (!t)
		return NULL;
	for (size_t i = 0; i < n; i++) {
		uint8_t *b = malloc(1 + c[i].data_bytes);

		if (!b || c[i].data_bytes > 254) {
			free(b);
			return NULL;
		}
		b[0] = c[i].cmd;
		if (c[i].data_bytes)
			memcpy(&b[1], c[i].data, c[i].data_bytes);
		g.bufs = realloc(g.bufs, (g.nbufs + 1) * sizeof(*g.bufs));
		g.bufs[g.nbufs++] = b;
		t[i] = (struct dpy_cmd){ DPY_CMD_DCS, (uint8_t)(1 + c[i].data_bytes), (uint16_t)c[i].delay_ms, b };
	}
	return t;
}

void f101_display_graph_clear(void)
{
	for (size_t i = 0; i < g.nbufs; i++)
		free(g.bufs[i]);
	free(g.bufs);
	free(g.init);
	free(g.exit);
	memset(&g, 0, sizeof(g));
}

const struct dpy_board_desc *f101_display_board(void)
{
	return g.valid ? &g.board : NULL;
}

int f101_display_graph_set(const f101_display_desc_t *d)
{
	uint8_t n = 0;
	struct dpy_node *nd = g.nodes;

	f101_display_graph_clear();
	g.desc = *d;
	nd[n++] = (struct dpy_node){ .name = DE_NAME, .compatible = c_compat_0,
		.status = DPY_STATUS_OKAY, .res = de_res, .nres = DPY_ARRAY_SIZE(de_res), .pdata = &de_pdata };
	nd[n++] = (struct dpy_node){ .name = TOP_NAME,
		.compatible = c_compat_1,
		.status = DPY_STATUS_OKAY, .res = top_res, .nres = DPY_ARRAY_SIZE(top_res) };
	nd[n++] = (struct dpy_node){ .name = LCD_NAME,
		.compatible = c_compat_2,
		.status = DPY_STATUS_OKAY, .res = lcd_res, .nres = DPY_ARRAY_SIZE(lcd_res),
		.refs = lcd_refs, .nrefs = DPY_ARRAY_SIZE(lcd_refs) };

	if (!d->dsi) {
		memset(&g.rgb_pdata, 0, sizeof(g.rgb_pdata));
		g.rgb_pdata.pins = (struct dpy_pin_group){ .pins = g.desc.pins, .count = g.desc.npins };
		g.rgb_pdata.hv_mode = DPY_HV_PARALLEL_RGB;
		g.rgb_pdata.rgb_swap = 5;
		g.simple = (struct dpy_panel_simple_pdata){
			.name = PANEL_NAME, .modes = &g.desc.mode, .num_modes = 1,
			.width_mm = d->width_mm, .height_mm = d->height_mm,
			.bus_format = d->bus_format, .bus_flags = d->bus_flags,
			.enable_delay_ms = d->enable_delay_ms,
		};
		nd[n++] = (struct dpy_node){ .name = RGB_NAME, .compatible = c_compat_3,
			.status = DPY_STATUS_OKAY, .pdata = &g.rgb_pdata };
		nd[n++] = (struct dpy_node){ .name = PANEL_NAME, .compatible = c_compat_4,
			.status = DPY_STATUS_OKAY, .pdata = &g.simple };
		g.links[0] = (struct dpy_link){ DE_NAME, 0, 0, LCD_NAME, 0, 0 };
		g.links[1] = (struct dpy_link){ LCD_NAME, 1, 0, RGB_NAME, 0, 0 };
		g.links[2] = (struct dpy_link){ RGB_NAME, 1, 0, PANEL_NAME, 0, 0 };
	} else {
		/* the DSI lane pads are fixed by the SoC; drive level 3 */
		for (int i = 0; i < 10; i++)
			g.dsi_pins[i] = (struct dpy_pin)DPY_PIN_DRV(DPY_SUNXI_PIN('D', 12 + i), 4, 3);
		g.dsi_pdata.pins = (struct dpy_pin_group){ .pins = g.dsi_pins, .count = 10 };

		uint16_t hold = d->reset_hold_ms ? d->reset_hold_ms : 100;
		uint8_t pc = 0;

		if (d->reset_gpio >= 0) {
			g.power_on[pc++] = (struct dpy_cmd)DPY_CMD_SET_GPIO(d->reset_gpio, 1);
			g.power_on[pc++] = (struct dpy_cmd)DPY_CMD_DELAY_MS(hold);
			g.power_on[pc++] = (struct dpy_cmd)DPY_CMD_SET_GPIO(d->reset_gpio, 0);
			g.power_on[pc++] = (struct dpy_cmd)DPY_CMD_DELAY_MS(hold);
			g.power_on[pc++] = (struct dpy_cmd)DPY_CMD_SET_GPIO(d->reset_gpio, 1);
			g.power_on[pc++] = (struct dpy_cmd)DPY_CMD_DELAY_MS(hold);
			g.power_off[0] = (struct dpy_cmd)DPY_CMD_SET_GPIO(d->reset_gpio, 0);
		}
		g.init = dcs_table(d->init_cmds, d->n_init);
		g.exit = dcs_table(d->exit_cmds, d->n_exit);
		if ((d->n_init && !g.init) || (d->n_exit && !g.exit)) {
			f101_display_graph_clear();
			return -1;
		}
		g.dsi_panel = (struct dpy_panel_dsi_pdata){
			.name = PANEL_NAME, .modes = &g.desc.mode, .num_modes = 1,
			.width_mm = d->width_mm, .height_mm = d->height_mm,
			.lanes = d->lanes ? d->lanes : 4, .format = DPY_DSI_FMT_RGB888, .mode_flags = d->dsi_mode_flags,
			.power_on = { g.power_on, pc },
			.power_off = { g.power_off, (uint16_t)(d->reset_gpio >= 0 ? 1 : 0) },
			.init = { g.init, (uint16_t)d->n_init },
			.exit = { g.exit, (uint16_t)d->n_exit },
			.enable_delay_ms = d->enable_delay_ms,
		};
		nd[n++] = (struct dpy_node){ .name = DSI_NAME,
			.compatible = c_compat_5,
			.status = DPY_STATUS_OKAY, .res = dsi_res, .nres = DPY_ARRAY_SIZE(dsi_res),
			.refs = dsi_refs, .nrefs = DPY_ARRAY_SIZE(dsi_refs), .pdata = &g.dsi_pdata };
		nd[n++] = (struct dpy_node){ .name = DPHY_NAME,
			.compatible = c_compat_6,
			.status = DPY_STATUS_OKAY, .res = dphy_res, .nres = DPY_ARRAY_SIZE(dphy_res),
			.pdata = &dpy_sun252iw2_dphy_pdata };
		nd[n++] = (struct dpy_node){ .name = PANEL_NAME, .compatible = c_compat_7,
			.status = DPY_STATUS_OKAY, .pdata = &g.dsi_panel };
		g.links[0] = (struct dpy_link){ DE_NAME, 0, 0, LCD_NAME, 0, 0 };
		g.links[1] = (struct dpy_link){ LCD_NAME, 1, 2, DSI_NAME, 0, 0 };
		g.links[2] = (struct dpy_link){ DSI_NAME, 1, 0, PANEL_NAME, 0, 0 };
	}
	g.board = (struct dpy_board_desc){ .name = d->dsi ? "dsi" : "rgb", .nodes = nd, .nnodes = n, .links = g.links, .nlinks = 3 };
	g.valid = true;
	return 0;
}
