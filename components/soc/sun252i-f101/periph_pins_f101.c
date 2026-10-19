/*
 * SPDX-FileCopyrightText: 2026 Yuzuki Tsuru
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stddef.h>
#include "soc/f101_periph_pins.h"
#include "soc/sun252i_f101_ll.h"

#define PB(n)   (32 + (n))
#define PC(n)   (64 + (n))
#define PD(n)   (96 + (n))
#define PE(n)   (128 + (n))
#define PF(n)   (160 + (n))
#define OPT(p, m)   { (p), (m) }
#define N(a)        ((int)(sizeof(a) / sizeof((a)[0])))

/* ---- UART ---- */
static const f101_pin_option_t u0_tx[] = { OPT(PF(2), 3) };
static const f101_pin_option_t u0_rx[] = { OPT(PF(4), 3) };
static const f101_pin_option_t u1_tx[] = { OPT(PF(0), 4), OPT(PB(0), 4) };
static const f101_pin_option_t u1_rx[] = { OPT(PF(1), 4), OPT(PB(1), 4) };
static const f101_pin_option_t u2_tx[] = { OPT(PF(4), 6), OPT(PE(6), 3) };
static const f101_pin_option_t u2_rx[] = { OPT(PF(5), 6), OPT(PE(7), 3) };
static const f101_pin_option_t u3_tx[] = { OPT(PE(8), 6) };
static const f101_pin_option_t u3_rx[] = { OPT(PE(9), 6) };
static const f101_pin_option_t u4_tx[] = { OPT(PE(2), 6), OPT(PD(6), 5) };
static const f101_pin_option_t u4_rx[] = { OPT(PE(3), 6), OPT(PD(7), 5) };
static const f101_pin_option_t u5_tx[] = { OPT(PE(4), 7) };
static const f101_pin_option_t u5_rx[] = { OPT(PE(5), 7) };

/* ---- I2C (TWI) ---- */
static const f101_pin_option_t i2c0_scl[] = { OPT(PE(8), 4) };
static const f101_pin_option_t i2c0_sda[] = { OPT(PE(9), 4) };
static const f101_pin_option_t i2c1_scl[] = { OPT(PE(0), 4), OPT(PD(0), 4) };
static const f101_pin_option_t i2c1_sda[] = { OPT(PE(1), 4), OPT(PD(1), 4) };
static const f101_pin_option_t i2c2_scl[] = { OPT(PD(10), 4) };
static const f101_pin_option_t i2c2_sda[] = { OPT(PD(11), 4) };

/* ---- SPI ---- */
static const f101_pin_option_t spi0_mosi[] = { OPT(PC(4), 3) };
static const f101_pin_option_t spi0_miso[] = { OPT(PC(2), 3) };
static const f101_pin_option_t spi0_sclk[] = { OPT(PC(0), 3) };
static const f101_pin_option_t spi0_wp[]   = { OPT(PC(3), 3) };
static const f101_pin_option_t spi0_hold[] = { OPT(PC(5), 3) };
static const f101_pin_option_t spi0_cs[]   = { OPT(PC(1), 3) };
static const f101_pin_option_t spi1_mosi[] = { OPT(PD(2), 3) };
static const f101_pin_option_t spi1_miso[] = { OPT(PD(3), 3) };
static const f101_pin_option_t spi1_sclk[] = { OPT(PD(1), 3) };
static const f101_pin_option_t spi1_wp[]   = { OPT(PD(5), 3) };
static const f101_pin_option_t spi1_hold[] = { OPT(PD(4), 3) };
static const f101_pin_option_t spi1_cs[]   = { OPT(PD(0), 3) };

/* ---- SD/MMC host: SMHC0 on PF at mux 2, SMHC2 on PE at mux 9 ---- */
static const f101_pin_option_t sd0_clk[] = { OPT(PF(2), 2) };
static const f101_pin_option_t sd0_cmd[] = { OPT(PF(3), 2) };
static const f101_pin_option_t sd0_d0[]  = { OPT(PF(1), 2) };
static const f101_pin_option_t sd0_d1[]  = { OPT(PF(0), 2) };
static const f101_pin_option_t sd0_d2[]  = { OPT(PF(5), 2) };
static const f101_pin_option_t sd0_d3[]  = { OPT(PF(4), 2) };
static const f101_pin_option_t sd2_clk[] = { OPT(PE(2), 9) };
static const f101_pin_option_t sd2_cmd[] = { OPT(PE(3), 9) };
static const f101_pin_option_t sd2_d0[]  = { OPT(PE(1), 9) };
static const f101_pin_option_t sd2_d1[]  = { OPT(PE(0), 9) };
static const f101_pin_option_t sd2_d2[]  = { OPT(PE(5), 9) };
static const f101_pin_option_t sd2_d3[]  = { OPT(PE(4), 9) };

/* ---- PWM channels ---- */
static const f101_pin_option_t pwm0[] = { OPT(PD(6), 6) };
static const f101_pin_option_t pwm1[] = { OPT(PD(7), 6) };
static const f101_pin_option_t pwm2[] = { OPT(PD(8), 6), OPT(PD(22), 4) };
static const f101_pin_option_t pwm3[] = { OPT(PB(3), 7) };

/* ---- backlight PWM outputs ---- */
static const f101_pin_option_t pbl0[] = { OPT(PB(0), 2) };
static const f101_pin_option_t pbl1[] = { OPT(PB(1), 2) };
static const f101_pin_option_t pbl2[] = { OPT(PB(2), 2) };
static const f101_pin_option_t pbl3[] = { OPT(PB(3), 2) };

/* ---- display: the parallel RGB bus uses PD0..PD9 and PD12..PD21 at mux 2, PE6/PE7 at mux 4; the
 * DSI link the pads PD12..PD21 at mux 4 ---- */
static const f101_pin_option_t lcd_rgb[] = {
    OPT(PD(0), 2),  OPT(PD(1), 2),  OPT(PD(2), 2),  OPT(PD(3), 2),  OPT(PD(4), 2),  OPT(PD(5), 2),  OPT(PD(6), 2),
    OPT(PD(7), 2),  OPT(PD(8), 2),  OPT(PD(9), 2),  OPT(PD(12), 2), OPT(PD(13), 2), OPT(PD(14), 2), OPT(PD(15), 2),
    OPT(PD(16), 2), OPT(PD(17), 2), OPT(PD(18), 2), OPT(PD(19), 2), OPT(PD(20), 2), OPT(PD(21), 2), OPT(PE(6), 4),
    OPT(PE(7), 4),
};
static const f101_pin_option_t lcd_dsi[] = {
    OPT(PD(12), 4), OPT(PD(13), 4), OPT(PD(14), 4), OPT(PD(15), 4), OPT(PD(16), 4), OPT(PD(17), 4), OPT(PD(18), 4),
    OPT(PD(19), 4), OPT(PD(20), 4), OPT(PD(21), 4),
};

typedef struct {
    const f101_pin_option_t *opt;
    uint8_t n;
} list_t;

#define L(a)    { a, N(a) }
#define NONE    { NULL, 0 }

static const list_t s_uart_tx[] = { L(u0_tx), L(u1_tx), L(u2_tx), L(u3_tx), L(u4_tx), L(u5_tx) };
static const list_t s_uart_rx[] = { L(u0_rx), L(u1_rx), L(u2_rx), L(u3_rx), L(u4_rx), L(u5_rx) };
static const list_t s_i2c_scl[] = { L(i2c0_scl), L(i2c1_scl), L(i2c2_scl) };
static const list_t s_i2c_sda[] = { L(i2c0_sda), L(i2c1_sda), L(i2c2_sda) };
static const list_t s_spi_mosi[] = { L(spi0_mosi), L(spi1_mosi) };
static const list_t s_spi_miso[] = { L(spi0_miso), L(spi1_miso) };
static const list_t s_spi_sclk[] = { L(spi0_sclk), L(spi1_sclk) };
static const list_t s_spi_wp[]   = { L(spi0_wp), L(spi1_wp) };
static const list_t s_spi_hold[] = { L(spi0_hold), L(spi1_hold) };
static const list_t s_spi_cs[]   = { L(spi0_cs), L(spi1_cs) };
/* SMHC1 does not exist: instance 1 is empty */
static const list_t s_sd_clk[] = { L(sd0_clk), NONE, L(sd2_clk) };
static const list_t s_sd_cmd[] = { L(sd0_cmd), NONE, L(sd2_cmd) };
static const list_t s_sd_d0[]  = { L(sd0_d0), NONE, L(sd2_d0) };
static const list_t s_sd_d1[]  = { L(sd0_d1), NONE, L(sd2_d1) };
static const list_t s_sd_d2[]  = { L(sd0_d2), NONE, L(sd2_d2) };
static const list_t s_sd_d3[]  = { L(sd0_d3), NONE, L(sd2_d3) };
static const list_t s_pwm[] = { L(pwm0), L(pwm1), L(pwm2), L(pwm3) };
static const list_t s_pwm_bl[] = { L(pbl0), L(pbl1), L(pbl2), L(pbl3) };
static const list_t s_lcd_rgb[] = { L(lcd_rgb) };
static const list_t s_lcd_dsi[] = { L(lcd_dsi) };

typedef struct {
    const list_t *lists;
    uint8_t count;
} sig_t;

#define S(a)    { a, N(a) }

static const sig_t s_sig[F101_SIG_MAX] = {
    [F101_SIG_UART_TX] = S(s_uart_tx),   [F101_SIG_UART_RX] = S(s_uart_rx),
    [F101_SIG_I2C_SCL] = S(s_i2c_scl),   [F101_SIG_I2C_SDA] = S(s_i2c_sda),
    [F101_SIG_SPI_MOSI] = S(s_spi_mosi), [F101_SIG_SPI_MISO] = S(s_spi_miso), [F101_SIG_SPI_SCLK] = S(s_spi_sclk),
    [F101_SIG_SPI_WP] = S(s_spi_wp),     [F101_SIG_SPI_HOLD] = S(s_spi_hold), [F101_SIG_SPI_CS] = S(s_spi_cs),
    [F101_SIG_SMHC_CLK] = S(s_sd_clk),   [F101_SIG_SMHC_CMD] = S(s_sd_cmd),
    [F101_SIG_SMHC_D0] = S(s_sd_d0),     [F101_SIG_SMHC_D1] = S(s_sd_d1),
    [F101_SIG_SMHC_D2] = S(s_sd_d2),     [F101_SIG_SMHC_D3] = S(s_sd_d3),
    [F101_SIG_PWM] = S(s_pwm),
    [F101_SIG_PWM_BL] = S(s_pwm_bl),
    [F101_SIG_LCD_RGB] = S(s_lcd_rgb),
    [F101_SIG_LCD_DSI] = S(s_lcd_dsi),
};

const f101_pin_option_t *f101_periph_pin_options(f101_signal_t sig, int instance, int *count)
{
    *count = 0;
    if ((unsigned)sig >= F101_SIG_MAX || instance < 0 || instance >= s_sig[sig].count) {
        return NULL;
    }
    *count = s_sig[sig].lists[instance].n;
    return s_sig[sig].lists[instance].opt;
}

int f101_periph_pin_mux(f101_signal_t sig, int instance, int gpio)
{
    int n;
    const f101_pin_option_t *o = f101_periph_pin_options(sig, instance, &n);

    for (int i = 0; i < n; i++) {
        if (o[i].pin == gpio) {
            return o[i].mux;
        }
    }
    return -1;
}

esp_err_t f101_periph_route(f101_signal_t sig, int instance, int gpio)
{
    int mux = f101_periph_pin_mux(sig, instance, gpio);

    if (mux < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    f101_pin_mux(gpio, (uint32_t)mux);
    return ESP_OK;
}
