/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */
#pragma once

/**
 * Mock definitions for running on the host.
 */

/**
 * @brief Supported clock sources for modules (CPU, peripherals, RTC, etc.)
 *
 * @note enum starts from 1, to save 0 for special purpose
 */
typedef enum {
    SOC_MOD_CLK_APB = 1,
} soc_module_clk_t;

/**
 * @brief Type of SPI clock source.
 */
typedef enum {
    SPI_CLK_SRC_DEFAULT = SOC_MOD_CLK_APB,
    SPI_CLK_SRC_APB = SOC_MOD_CLK_APB,
} soc_periph_spi_clk_src_t;

typedef enum {
    UART_SCLK_DEFAULT = SOC_MOD_CLK_APB,
    UART_SCLK_APB = SOC_MOD_CLK_APB,
} soc_periph_uart_clk_src_legacy_t;

typedef enum {
    I2C_CLK_SRC_DEFAULT = SOC_MOD_CLK_APB,
    I2C_CLK_SRC_APB = SOC_MOD_CLK_APB,
} soc_periph_i2c_clk_src_t;

typedef enum {
    GPTIMER_CLK_SRC_DEFAULT = SOC_MOD_CLK_APB,
    GPTIMER_CLK_SRC_APB = SOC_MOD_CLK_APB,
} soc_periph_gptimer_clk_src_t;

/* the PWM block runs from its own 24 MHz / 100 MHz sources, selected by the driver */
#define LEDC_USE_RC_FAST_CLK    1
#define LEDC_USE_APB_CLK        2
typedef enum {
    LEDC_AUTO_CLK = 0,
    LEDC_USE_RC_FAST_CLK_ENUM = LEDC_USE_RC_FAST_CLK,
} soc_periph_ledc_clk_src_legacy_t;

/* the pixel clock of the LCD is derived from the display timing by the display driver */
typedef enum {
    LCD_CLK_SRC_DEFAULT = SOC_MOD_CLK_APB,
} soc_periph_lcd_clk_src_t;

/* the audio codec runs from PLL_AUDIO1, programmed by the I2S driver for the sample rate family */
typedef enum {
    I2S_CLK_SRC_DEFAULT = SOC_MOD_CLK_APB,
    I2S_CLK_SRC_PLL_AUDIO = SOC_MOD_CLK_APB,
} soc_periph_i2s_clk_src_t;
