/*
 * SPDX-FileCopyrightText: 2026 Yuzuki Tsuru
 * SPDX-License-Identifier: Apache-2.0
 *
 * JD9168S MIPI DSI panel controller (1024x768 IPS panel of the YuzukiNeko carrier board).
 */
#pragma once

#include "esp_lcd_panel_dev.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_dpi_f101.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Vendor configuration of the panel; the initialisation table is optional. */
typedef struct {
    const esp_lcd_dpi_f101_cmd_t *init_cmds;            /*!< replaces the built-in table when not NULL */
    uint16_t init_cmds_size;
    struct {
        esp_lcd_dsi_bus_handle_t dsi_bus;               /*!< the DSI bus */
        const esp_lcd_dpi_panel_config_t *dpi_config;   /*!< video timing, frame buffers */
    } mipi_config;
} jd9168s_vendor_config_t;

/**
 * @brief Create the panel: a DPI panel on the DSI bus with the controller's reset line and
 *        initialisation commands. Use it like the other esp_lcd panels (reset, init, draw_bitmap ...).
 *
 * @param io                DBI io of the DSI bus
 * @param panel_dev_config  reset_gpio_num is the panel reset (active low); vendor_config is a
 *                          jd9168s_vendor_config_t
 */
esp_err_t esp_lcd_new_panel_jd9168s(const esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *panel_dev_config,
                                    esp_lcd_panel_handle_t *ret_panel);

/** The video timing of the panel: 1024x768 at 54 MHz. */
#define JD9168S_1024_768_PANEL_TIMING() { \
    .h_size = 1024, .v_size = 768, \
    .hsync_pulse_width = 10, .hsync_back_porch = 40, .hsync_front_porch = 40, \
    .vsync_pulse_width = 8, .vsync_back_porch = 21, .vsync_front_porch = 12, \
}
#define JD9168S_DPI_CLOCK_MHZ   54.0f

#ifdef __cplusplus
}
#endif
