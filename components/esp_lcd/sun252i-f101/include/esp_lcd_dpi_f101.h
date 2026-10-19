/*
 * SPDX-FileCopyrightText: 2026 Yuzuki Tsuru
 * SPDX-License-Identifier: Apache-2.0
 *
 * What a MIPI DSI panel driver adds to the stock DPI panel (esp_lcd_new_panel_dpi): the commands that
 * bring the panel controller up (sent over the DSI link in low power mode while the display starts),
 * the reset line and the link flags. A vendor panel driver calls this once between creating the DPI
 * panel and esp_lcd_panel_init().
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_lcd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* link flags (mode_flags) */
#define F101_DSI_MODE_VIDEO             (1U << 0)
#define F101_DSI_MODE_VIDEO_BURST       (1U << 1)
#define F101_DSI_MODE_VIDEO_SYNC_PULSE  (1U << 2)
#define F101_DSI_MODE_LPM               (1U << 3)   /*!< commands in low power mode */
#define F101_DSI_CLOCK_NON_CONTINUOUS   (1U << 4)

/** One DCS command: the command byte, its parameters, and a delay after it (same shape as the init
 *  tables of the other esp_lcd panel drivers). */
typedef struct {
    uint8_t cmd;
    const uint8_t *data;
    size_t data_bytes;
    uint32_t delay_ms;
} esp_lcd_dpi_f101_cmd_t;

typedef struct {
    const esp_lcd_dpi_f101_cmd_t *init_cmds;    /*!< sent after the link is up, before the video starts */
    size_t init_cmds_size;
    const esp_lcd_dpi_f101_cmd_t *exit_cmds;    /*!< sent when the display is switched off */
    size_t exit_cmds_size;
    int reset_gpio_num;                         /*!< panel reset (active low), -1 if there is none */
    uint16_t reset_hold_ms;                     /*!< time of each reset step, 0: 100 ms */
    uint32_t mode_flags;                        /*!< F101_DSI_MODE_*, 0: video, sync pulses, commands in LP */
    uint16_t width_mm;                          /*!< physical size, 0 if unknown */
    uint16_t height_mm;
    uint16_t enable_delay_ms;                   /*!< video running -> display on */
} esp_lcd_dpi_f101_extra_t;

esp_err_t esp_lcd_dpi_panel_f101_setup(esp_lcd_panel_handle_t dpi_panel, const esp_lcd_dpi_f101_extra_t *extra);

/**
 * @brief Backlight of a panel (RGB or DPI): esp_lcd_panel_set_brightness() calls @p set with the level
 *        0..255. The backlight is a board matter (LEDC, a PWM_BL output, a DCS command); the board support
 *        code registers it here.
 */
typedef esp_err_t (*esp_lcd_f101_backlight_cb_t)(void *ctx, int level);

esp_err_t esp_lcd_panel_f101_set_backlight(esp_lcd_panel_handle_t panel, esp_lcd_f101_backlight_cb_t set, void *ctx);

#ifdef __cplusplus
}
#endif
