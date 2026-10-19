/*
 * SPDX-FileCopyrightText: 2026 Yuzuki Tsuru
 * SPDX-License-Identifier: Apache-2.0
 *
 * Goodix GT967 capacitive touch controller (the register map is that of the GT911 family).
 */
#pragma once

#include "esp_lcd_touch.h"
#include "esp_lcd_panel_io.h"

#ifdef __cplusplus
extern "C" {
#endif

/** I2C panel io configuration: address 0x5d (INT low at reset), 16 bit register addresses */
#define ESP_LCD_TOUCH_IO_I2C_GT967_ADDRESS  0x5d
#define ESP_LCD_TOUCH_IO_I2C_GT967_CONFIG() { \
    .dev_addr = ESP_LCD_TOUCH_IO_I2C_GT967_ADDRESS, \
    .control_phase_bytes = 1, \
    .dc_bit_offset = 0, \
    .lcd_cmd_bits = 16, \
    .flags = { .disable_control_phase = 1 }, \
    .scl_speed_hz = 400000, \
}

/**
 * @brief Create the touch driver on an I2C panel io. The controller is reset, its product id is
 *        checked and its resolution and number of touch points are set from the configuration
 *        (x_max, y_max, CONFIG_ESP_LCD_TOUCH_MAX_POINTS). The interrupt line is not used: the data
 *        is read when esp_lcd_touch_read_data() is called.
 */
esp_err_t esp_lcd_touch_new_i2c_gt967(const esp_lcd_panel_io_handle_t io, const esp_lcd_touch_config_t *config,
                                      esp_lcd_touch_handle_t *out_touch);

#ifdef __cplusplus
}
#endif
