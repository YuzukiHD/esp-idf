/*
 * SPDX-FileCopyrightText: 2026 Yuzuki Tsuru
 * SPDX-License-Identifier: Apache-2.0
 */
#include "esp_check.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_jd9168s.h"
#include "jd9168s_init.h"

static const char *TAG = "jd9168s";

esp_err_t esp_lcd_new_panel_jd9168s(const esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *dev, esp_lcd_panel_handle_t *ret_panel)
{
    ESP_RETURN_ON_FALSE(dev && dev->vendor_config && ret_panel, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    const jd9168s_vendor_config_t *vc = dev->vendor_config;

    ESP_RETURN_ON_FALSE(vc->mipi_config.dsi_bus && vc->mipi_config.dpi_config, ESP_ERR_INVALID_ARG, TAG, "no DSI bus or DPI configuration");
    esp_lcd_panel_handle_t panel;

    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_dpi(vc->mipi_config.dsi_bus, vc->mipi_config.dpi_config, &panel), TAG, "dpi panel");
    const esp_lcd_dpi_f101_extra_t extra = {
        .init_cmds = vc->init_cmds ? vc->init_cmds : jd9168s_init_cmds,
        .init_cmds_size = vc->init_cmds ? vc->init_cmds_size : sizeof(jd9168s_init_cmds) / sizeof(jd9168s_init_cmds[0]),
        .exit_cmds = jd9168s_exit_cmds,
        .exit_cmds_size = sizeof(jd9168s_exit_cmds) / sizeof(jd9168s_exit_cmds[0]),
        .reset_gpio_num = dev->reset_gpio_num,
        .width_mm = 65,
        .height_mm = 48,
    };
    esp_err_t e = esp_lcd_dpi_panel_f101_setup(panel, &extra);

    if (e != ESP_OK) {
        esp_lcd_panel_del(panel);
        return e;
    }
    *ret_panel = panel;
    return ESP_OK;
}
