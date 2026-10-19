/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later */
/*
 * What the panel front ends (RGB, DPI) tell the display stack: the output, its pads, the timing and
 * the commands of the panel. f101_display_graph_set() turns it into the pipeline graph.
 */
#ifndef F101_DISPLAY_DESC_H
#define F101_DISPLAY_DESC_H

#include <stdbool.h>
#include <stdint.h>
#include <dpy/dpy_graph.h>
#include <dpy/dpy_pdata.h>
#include "esp_lcd_dpi_f101.h"

#define F101_DISP_MAX_PINS  32

typedef struct {
    bool dsi;                                   /* MIPI DSI, else parallel RGB */
    struct dpy_display_mode mode;
    uint32_t width_mm;
    uint32_t height_mm;
    uint16_t enable_delay_ms;
    /* parallel RGB */
    struct dpy_pin pins[F101_DISP_MAX_PINS];
    uint8_t npins;
    uint32_t bus_format;                        /* enum dpy_bus_format */
    uint32_t bus_flags;                         /* DPY_BUS_FLAG_* */
    /* MIPI DSI */
    uint8_t lanes;
    uint32_t dsi_mode_flags;
    int reset_gpio;
    uint16_t reset_hold_ms;
    const esp_lcd_dpi_f101_cmd_t *init_cmds;
    size_t n_init;
    const esp_lcd_dpi_f101_cmd_t *exit_cmds;
    size_t n_exit;
} f101_display_desc_t;

/** Build the graph for @p desc (replaces an earlier one); the descriptor and the command tables it
 *  points to must stay valid until f101_display_graph_clear(). */
int f101_display_graph_set(const f101_display_desc_t *desc);
void f101_display_graph_clear(void);
const struct dpy_board_desc *f101_display_board(void);

#endif
