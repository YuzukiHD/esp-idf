/*
 * SPDX-FileCopyrightText: 2026 Yuzuki Tsuru
 * SPDX-License-Identifier: Apache-2.0
 *
 * Which pads can carry which peripheral signal. The SoC has no GPIO matrix: every pad has a few fixed
 * alternate functions (mux values), so each signal lists the pads (and the mux that selects it) it can
 * be routed to. Drivers take the pad from their configuration structure and ask this table for the mux;
 * a pad that cannot carry the signal is refused. Nothing here is board specific.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    F101_SIG_UART_TX,       /*!< instance: UART number */
    F101_SIG_UART_RX,
    F101_SIG_I2C_SCL,       /*!< instance: TWI number */
    F101_SIG_I2C_SDA,
    F101_SIG_SPI_MOSI,      /*!< instance: SPI controller */
    F101_SIG_SPI_MISO,
    F101_SIG_SPI_SCLK,
    F101_SIG_SPI_WP,
    F101_SIG_SPI_HOLD,
    F101_SIG_SPI_CS,
    F101_SIG_SMHC_CLK,      /*!< instance: SD/MMC host controller */
    F101_SIG_SMHC_CMD,
    F101_SIG_SMHC_D0,
    F101_SIG_SMHC_D1,
    F101_SIG_SMHC_D2,
    F101_SIG_SMHC_D3,
    F101_SIG_PWM,           /*!< instance: PWM channel */
    F101_SIG_PWM_BL,        /*!< instance: backlight PWM output (analog current sink) */
    F101_SIG_LCD_RGB,       /*!< instance 0: any pad of the parallel RGB bus (clock, syncs, enable and data) */
    F101_SIG_LCD_DSI,       /*!< instance 0: the clock and data lane pads of the MIPI DSI link */
    F101_SIG_MAX,
} f101_signal_t;

typedef struct {
    uint8_t pin;            /*!< bank * 32 + number (PA = 0, PB = 32, ...) */
    uint8_t mux;            /*!< function that selects the signal on the pad */
} f101_pin_option_t;

/**
 * @brief The pads a signal can be routed to.
 *
 * @param[out] count number of entries, 0 when the instance has no such signal
 * @return the options (NULL when there are none)
 */
const f101_pin_option_t *f101_periph_pin_options(f101_signal_t sig, int instance, int *count);

/**
 * @return the mux that routes the signal to the pad, -1 when the pad cannot carry it
 */
int f101_periph_pin_mux(f101_signal_t sig, int instance, int gpio);

/**
 * @brief Route a signal to a pad: select the mux. The pull and drive settings are left to the caller.
 *
 * @return ESP_ERR_INVALID_ARG when the pad cannot carry the signal
 */
esp_err_t f101_periph_route(f101_signal_t sig, int instance, int gpio);

#ifdef __cplusplus
}
#endif
