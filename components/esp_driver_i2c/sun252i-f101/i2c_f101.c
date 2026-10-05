/*
 * SPDX-FileCopyrightText: 2026 Yuzuki Tsuru
 * SPDX-License-Identifier: Apache-2.0
 *
 * driver/i2c_master.h on the Allwinner TWI controllers (polled, 7 bit
 * addresses). Both buses take the 24 MHz reference as module clock.
 */

#include <string.h>
#include <stdlib.h>
#include "sdkconfig.h"
#include "esp_check.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "soc/sun252i_f101_ll.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define TWI_BASE(n)     (0x02502000u + (n) * 0x400u)
#define TWI_BGR         (F101_CCU_BASE + 0x91cu)
#define TWI_DATA        0x08u
#define TWI_CTL         0x0cu
#define TWI_STATUS      0x10u
#define TWI_CLK         0x14u
#define TWI_SRST        0x18u
#define TWI_EFT         0x1cu

#define CTL_ACK         (1u << 2)
#define CTL_INTFLG      (1u << 3)
#define CTL_STP         (1u << 4)
#define CTL_STA         (1u << 5)
#define CTL_BUSEN       (1u << 6)

#define ST_START        0x08u
#define ST_RESTART      0x10u
#define ST_ADDR_W       0x18u
#define ST_ADDR_W_NACK  0x20u
#define ST_DATA_W       0x28u
#define ST_ARB_LOST     0x38u
#define ST_ADDR_R       0x40u
#define ST_ADDR_R_NACK  0x48u
#define ST_DATA_R_ACK   0x50u
#define ST_DATA_R_NACK  0x58u

#define I2C_PIN_MUX     4

static const char *TAG = "i2c";

struct i2c_master_bus_t {
    int port;
    uint32_t base;
    SemaphoreHandle_t lock;
    uint32_t clk_reg;
    int ndev;
};

struct i2c_master_dev_t {
    struct i2c_master_bus_t *bus;
    uint16_t addr;
    uint32_t speed_hz;
    bool no_ack_check;
};

static struct i2c_master_bus_t *s_bus[SOC_I2C_NUM];
uint32_t f101_i2c_last_status, f101_i2c_last_step, f101_i2c_last_ctl, f101_i2c_last_lines;

static inline uint32_t rd(struct i2c_master_bus_t *b, uint32_t off)
{
    return F101_REG32(b->base + off);
}

static inline void wr(struct i2c_master_bus_t *b, uint32_t off, uint32_t v)
{
    F101_REG32(b->base + off) = v;
}

/* closest TWI_CLK value for the bus frequency: f = 24 MHz / (10 * (M + 1) * 2^N) */
static uint32_t clk_for(uint32_t hz)
{
    uint32_t best = 0, best_err = UINT32_MAX;

    for (uint32_t n = 0; n < 8; n++) {
        for (uint32_t m = 0; m < 16; m++) {
            uint32_t f = 24000000u / (10u * (m + 1) * (1u << n));
            uint32_t err = f > hz ? f - hz : hz - f;

            if (f <= hz && err < best_err) {
                best_err = err;
                best = (m << 3) | n;
            }
        }
    }
    return best;
}

static esp_err_t wait_flag(struct i2c_master_bus_t *b, uint32_t timeout_us, uint32_t *status)
{
    uint64_t end = f101_mtime_get() + (uint64_t)timeout_us * (F101_MTIME_HZ / 1000000u);

    while (!(rd(b, TWI_CTL) & CTL_INTFLG)) {
        if (f101_mtime_get() > end) {
            f101_i2c_last_ctl = rd(b, TWI_CTL);
            f101_i2c_last_status = rd(b, TWI_STATUS) & 0xff;
            f101_i2c_last_lines = 0;
            return ESP_ERR_TIMEOUT;
        }
    }
    *status = rd(b, TWI_STATUS) & 0xff;
    f101_i2c_last_status = *status;
    return ESP_OK;
}

static void bus_init_regs(struct i2c_master_bus_t *b, uint32_t hz)
{
    b->clk_reg = clk_for(hz);
    wr(b, TWI_EFT, 0);
    wr(b, TWI_SRST, 1);
    wr(b, TWI_CLK, b->clk_reg);
    wr(b, TWI_CTL, CTL_BUSEN);
}

static esp_err_t twi_stop(struct i2c_master_bus_t *b)
{
    wr(b, TWI_CTL, rd(b, TWI_CTL) | CTL_STP | CTL_INTFLG);
    uint64_t end = f101_mtime_get() + 100000ull * (F101_MTIME_HZ / 1000000u);

    while (rd(b, TWI_CTL) & CTL_STP) {
        if (f101_mtime_get() > end) {
            return ESP_ERR_TIMEOUT;
        }
    }
    return ESP_OK;
}

/* returns ESP_OK, ESP_ERR_NOT_FOUND (address NACK), ESP_FAIL (other bus error) or ESP_ERR_TIMEOUT */
static esp_err_t twi_start_addr(struct i2c_master_bus_t *b, uint16_t addr, bool read, bool restart, uint32_t to_us)
{
    uint32_t st;
    esp_err_t r;

    f101_i2c_last_step = 1;
    f101_i2c_last_status = rd(b, TWI_STATUS) & 0xff;
    wr(b, TWI_CTL, rd(b, TWI_CTL) | CTL_INTFLG | CTL_STA);
    r = wait_flag(b, to_us, &st);
    if (r != ESP_OK) {
        return r;
    }
    f101_i2c_last_step = 2;
    if (st != (restart ? ST_RESTART : ST_START)) {
        return ESP_FAIL;
    }
    wr(b, TWI_DATA, ((uint32_t)addr << 1) | (read ? 1u : 0u));
    wr(b, TWI_CTL, rd(b, TWI_CTL) | CTL_INTFLG);
    r = wait_flag(b, to_us, &st);
    if (r != ESP_OK) {
        return r;
    }
    if (st == (read ? ST_ADDR_R_NACK : ST_ADDR_W_NACK)) {
        return ESP_ERR_NOT_FOUND;
    }
    return st == (read ? ST_ADDR_R : ST_ADDR_W) ? ESP_OK : ESP_FAIL;
}

static esp_err_t twi_write_bytes(struct i2c_master_bus_t *b, const uint8_t *d, size_t n, uint32_t to_us, bool check_ack)
{
    for (size_t i = 0; i < n; i++) {
        uint32_t st;

        wr(b, TWI_DATA, d[i]);
        wr(b, TWI_CTL, rd(b, TWI_CTL) | CTL_INTFLG);
        esp_err_t r = wait_flag(b, to_us, &st);
        if (r != ESP_OK) {
            return r;
        }
        if (check_ack && st != ST_DATA_W) {
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

static esp_err_t twi_read_bytes(struct i2c_master_bus_t *b, uint8_t *d, size_t n, uint32_t to_us)
{
    for (size_t i = 0; i < n; i++) {
        uint32_t st, ctl = rd(b, TWI_CTL);
        bool more = i + 1 < n;

        ctl = more ? (ctl | CTL_ACK) : (ctl & ~CTL_ACK);
        wr(b, TWI_CTL, ctl | CTL_INTFLG);
        esp_err_t r = wait_flag(b, to_us, &st);
        if (r != ESP_OK) {
            return r;
        }
        if (st != (more ? ST_DATA_R_ACK : ST_DATA_R_NACK)) {
            return ESP_FAIL;
        }
        d[i] = (uint8_t)rd(b, TWI_DATA);
    }
    return ESP_OK;
}

static uint32_t to_us(int ms)
{
    return ms < 0 ? 1000000u : (uint32_t)ms * 1000u;
}

/* one write and/or read transaction */
static esp_err_t xfer(struct i2c_master_bus_t *b, uint16_t addr, const uint8_t *w, size_t wn, uint8_t *r, size_t rn,
                      int timeout_ms, bool check_ack)
{
    esp_err_t e = ESP_OK;
    uint32_t t = to_us(timeout_ms);

    xSemaphoreTake(b->lock, portMAX_DELAY);
    bool restart = false;

    if (wn || !rn) {
        e = twi_start_addr(b, addr, false, false, t);
        if (e == ESP_OK && wn) {
            e = twi_write_bytes(b, w, wn, t, check_ack);
        }
        restart = true;
    }
    if (e == ESP_OK && rn) {
        e = twi_start_addr(b, addr, true, restart, t);
        if (e == ESP_OK) {
            e = twi_read_bytes(b, r, rn, t);
        }
    }
    if (e != ESP_OK) {
        twi_stop(b);
        if (e == ESP_ERR_NOT_FOUND) {
            e = check_ack ? ESP_FAIL : ESP_OK;
        }
        if (e == ESP_ERR_TIMEOUT) {
            bus_init_regs(b, 0);          /* free the controller, then restore the speed */
            wr(b, TWI_CLK, b->clk_reg);
        }
    } else {
        e = twi_stop(b);
    }
    xSemaphoreGive(b->lock);
    return e;
}

esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *cfg, i2c_master_bus_handle_t *ret)
{
    ESP_RETURN_ON_FALSE(cfg && ret, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    int port = cfg->i2c_port;

    if (port < 0) {
        for (int i = 0; i < SOC_I2C_NUM; i++) {
            if (!s_bus[i]) {
                port = i;
                break;
            }
        }
    }
    ESP_RETURN_ON_FALSE(port >= 0 && port < SOC_I2C_NUM, ESP_ERR_NOT_FOUND, TAG, "no free i2c port");
    ESP_RETURN_ON_FALSE(!s_bus[port], ESP_ERR_INVALID_STATE, TAG, "port in use");

    struct i2c_master_bus_t *b = calloc(1, sizeof(*b));
    ESP_RETURN_ON_FALSE(b, ESP_ERR_NO_MEM, TAG, "no memory");
    b->port = port;
    b->base = TWI_BASE(port);
    b->lock = xSemaphoreCreateMutex();

    /* bus clock gate and reset release */
    F101_REG32(TWI_BGR) &= ~(1u << (16 + port));
    F101_REG32(TWI_BGR) |= (1u << port);
    F101_REG32(TWI_BGR) |= (1u << (16 + port));

    if (cfg->scl_io_num >= 0) {
        f101_pin_mux(cfg->scl_io_num, I2C_PIN_MUX);
        if (cfg->flags.enable_internal_pullup) {
            f101_pin_pull(cfg->scl_io_num, F101_PULL_UP);
        }
    }
    if (cfg->sda_io_num >= 0) {
        f101_pin_mux(cfg->sda_io_num, I2C_PIN_MUX);
        if (cfg->flags.enable_internal_pullup) {
            f101_pin_pull(cfg->sda_io_num, F101_PULL_UP);
        }
    }
    bus_init_regs(b, 100000);
    s_bus[port] = b;
    *ret = b;
    return ESP_OK;
}

esp_err_t i2c_del_master_bus(i2c_master_bus_handle_t bus)
{
    ESP_RETURN_ON_FALSE(bus, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(bus->ndev == 0, ESP_ERR_INVALID_STATE, TAG, "devices still attached");
    wr(bus, TWI_CTL, 0);
    F101_REG32(TWI_BGR) &= ~((1u << bus->port) | (1u << (16 + bus->port)));
    s_bus[bus->port] = NULL;
    vSemaphoreDelete(bus->lock);
    free(bus);
    return ESP_OK;
}

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t *cfg,
                                    i2c_master_dev_handle_t *ret)
{
    ESP_RETURN_ON_FALSE(bus && cfg && ret, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(cfg->dev_addr_length == I2C_ADDR_BIT_LEN_7, ESP_ERR_NOT_SUPPORTED, TAG, "7 bit addresses only");
    struct i2c_master_dev_t *d = calloc(1, sizeof(*d));
    ESP_RETURN_ON_FALSE(d, ESP_ERR_NO_MEM, TAG, "no memory");
    d->bus = bus;
    d->addr = cfg->device_address;
    d->speed_hz = cfg->scl_speed_hz ? cfg->scl_speed_hz : 100000;
    d->no_ack_check = cfg->flags.disable_ack_check;
    bus->ndev++;
    *ret = d;
    return ESP_OK;
}

esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t d)
{
    ESP_RETURN_ON_FALSE(d, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    d->bus->ndev--;
    free(d);
    return ESP_OK;
}

static void set_speed(struct i2c_master_dev_t *d)
{
    uint32_t c = clk_for(d->speed_hz);

    if (c != d->bus->clk_reg) {
        d->bus->clk_reg = c;
        wr(d->bus, TWI_CLK, c);
    }
}

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t d, const uint8_t *w, size_t wn, int timeout_ms)
{
    ESP_RETURN_ON_FALSE(d && w && wn, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    set_speed(d);
    return xfer(d->bus, d->addr, w, wn, NULL, 0, timeout_ms, !d->no_ack_check);
}

esp_err_t i2c_master_receive(i2c_master_dev_handle_t d, uint8_t *r, size_t rn, int timeout_ms)
{
    ESP_RETURN_ON_FALSE(d && r && rn, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    set_speed(d);
    return xfer(d->bus, d->addr, NULL, 0, r, rn, timeout_ms, !d->no_ack_check);
}

esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t d, const uint8_t *w, size_t wn, uint8_t *r, size_t rn,
                                      int timeout_ms)
{
    ESP_RETURN_ON_FALSE(d && w && wn && r && rn, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    set_speed(d);
    return xfer(d->bus, d->addr, w, wn, r, rn, timeout_ms, !d->no_ack_check);
}

esp_err_t i2c_master_multi_buffer_transmit(i2c_master_dev_handle_t d, i2c_master_transmit_multi_buffer_info_t *bufs,
                                           size_t n, int timeout_ms)
{
    ESP_RETURN_ON_FALSE(d && bufs && n, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    size_t total = 0;

    for (size_t i = 0; i < n; i++) {
        total += bufs[i].buffer_size;
    }
    uint8_t *tmp = malloc(total);
    ESP_RETURN_ON_FALSE(tmp, ESP_ERR_NO_MEM, TAG, "no memory");
    size_t off = 0;
    for (size_t i = 0; i < n; i++) {
        memcpy(tmp + off, bufs[i].write_buffer, bufs[i].buffer_size);
        off += bufs[i].buffer_size;
    }
    esp_err_t e = i2c_master_transmit(d, tmp, total, timeout_ms);
    free(tmp);
    return e;
}

esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus, uint16_t address, int timeout_ms)
{
    ESP_RETURN_ON_FALSE(bus, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    esp_err_t e = ESP_OK;

    xSemaphoreTake(bus->lock, portMAX_DELAY);
    e = twi_start_addr(bus, address, false, false, to_us(timeout_ms));
    twi_stop(bus);
    xSemaphoreGive(bus->lock);
    return e == ESP_OK ? ESP_OK : (e == ESP_ERR_NOT_FOUND ? ESP_ERR_NOT_FOUND : e);
}

esp_err_t i2c_master_bus_reset(i2c_master_bus_handle_t bus)
{
    ESP_RETURN_ON_FALSE(bus, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    bus_init_regs(bus, 100000);
    wr(bus, TWI_CLK, bus->clk_reg);
    return ESP_OK;
}

esp_err_t i2c_master_bus_wait_all_done(i2c_master_bus_handle_t bus, int timeout_ms)
{
    (void)bus;
    (void)timeout_ms;
    return ESP_OK;      /* all transfers are synchronous */
}

esp_err_t i2c_master_get_bus_handle(i2c_port_num_t port_num, i2c_master_bus_handle_t *ret_handle)
{
    ESP_RETURN_ON_FALSE(port_num >= 0 && port_num < SOC_I2C_NUM && s_bus[port_num] && ret_handle, ESP_ERR_INVALID_ARG,
                        TAG, "invalid port");
    *ret_handle = s_bus[port_num];
    return ESP_OK;
}

esp_err_t i2c_master_device_change_address(i2c_master_dev_handle_t d, uint16_t new_address, int timeout_ms)
{
    (void)timeout_ms;
    ESP_RETURN_ON_FALSE(d, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    d->addr = new_address;
    return ESP_OK;
}
