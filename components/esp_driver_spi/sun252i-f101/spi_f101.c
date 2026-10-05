/*
 * SPDX-FileCopyrightText: 2026 Yuzuki Tsuru
 * SPDX-License-Identifier: Apache-2.0
 *
 * driver/spi_master.h on the Allwinner SPI controllers (polled, 8 bit
 * words). SPI2_HOST is controller 0 (the flash lines PC0..PC5), SPI3_HOST is
 * controller 1. The chip select is the controller's own (PC1 on SPI2_HOST) or
 * a GPIO.
 */

#include <string.h>
#include <stdlib.h>
#include "sdkconfig.h"
#include "esp_check.h"
#include "esp_log.h"
#include "driver/spi_master.h"
#include "soc/sun252i_f101_ll.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define SPI_BASE(c)     (0x04025000u + (c) * 0x1000u)
#define SPI_BGR         (F101_CCU_BASE + 0x96cu)
#define SPI_CLK_REG(c)  (F101_CCU_BASE + 0x940u + (c) * 4u)

#define SPI_GC          0x04u
#define SPI_TC          0x08u
#define SPI_INT_CTL     0x10u
#define SPI_INT_STA     0x14u
#define SPI_FIFO_CTL    0x18u
#define SPI_FIFO_STA    0x1cu
#define SPI_CLK_CTL     0x24u
#define SPI_SDC         0x28u
#define SPI_BURST_CNT   0x30u
#define SPI_TX_CNT      0x34u
#define SPI_BCC         0x38u
#define SPI_TXDATA      0x200u
#define SPI_RXDATA      0x300u

#define GC_EN           (1u << 0)
#define GC_MODE         (1u << 1)
#define GC_TP_EN        (1u << 7)
#define TC_CPHA         (1u << 0)
#define TC_CPOL         (1u << 1)
#define TC_SPOL         (1u << 2)
#define TC_SS_OWNER     (1u << 6)
#define TC_SS_LEVEL     (1u << 7)
#define TC_DHB          (1u << 8)
#define TC_DDB          (1u << 9)
#define TC_FBS          (1u << 12)
#define TC_SDM          (1u << 13)
#define TC_XCH          (1u << 31)
#define INT_ERR         ((1u << 8) | (1u << 9) | (1u << 10))
#define INT_TC          (1u << 12)
#define FIFO_TX_RST     (1u << 31)
#define FIFO_RX_RST     (1u << 15)

#define FIFO_DEPTH      64
#define MAX_BURST       0xffffffu
#define NUM_HOSTS       2
#define NATIVE_CS_PIN   33          /* PC1 */

static const char *TAG = "spi";

struct spi_bus_t {
    int ctrl;
    uint32_t base;
    SemaphoreHandle_t lock;
    int ndev;
    bool used;
};

struct spi_device_t {
    struct spi_bus_t *bus;
    spi_device_interface_config_t cfg;
    uint32_t actual_hz;
};

static struct spi_bus_t s_bus[NUM_HOSTS];

#define CTRL_OF(host) ((int)(host) - (int)SPI2_HOST)

static inline uint32_t rd(struct spi_bus_t *b, uint32_t off)
{
    return F101_REG32(b->base + off);
}

static inline void wr(struct spi_bus_t *b, uint32_t off, uint32_t v)
{
    F101_REG32(b->base + off) = v;
}

/* SCK = 24 MHz / 2^n, the highest rate not above the request */
static uint32_t set_clock(struct spi_bus_t *b, uint32_t hz)
{
    uint32_t n = 0;

    while (n < 15 && (24000000u >> n) > hz) {
        n++;
    }
    F101_REG32(SPI_CLK_REG(b->ctrl)) = (1u << 31) | (n << 8);       /* HOSC, gate on */
    wr(b, SPI_CLK_CTL, (rd(b, SPI_CLK_CTL) & ~((0xfu << 8) | (1u << 12) | 0xffu)) | (n << 8));
    return 24000000u >> n;
}

static esp_err_t reset_fifo(struct spi_bus_t *b)
{
    wr(b, SPI_FIFO_CTL, (rd(b, SPI_FIFO_CTL) | FIFO_TX_RST | FIFO_RX_RST) & ~((0xffu << 16) | 0xffu));
    for (int i = 0; i < 100000; i++) {
        if (!(rd(b, SPI_FIFO_CTL) & (FIFO_TX_RST | FIFO_RX_RST))) {
            return ESP_OK;
        }
        f101_delay_us(1);
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t spi_bus_initialize(spi_host_device_t host_id, const spi_bus_config_t *cfg, spi_dma_chan_t dma_chan)
{
    (void)dma_chan;
    ESP_RETURN_ON_FALSE(cfg, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    int c = CTRL_OF(host_id);
    ESP_RETURN_ON_FALSE(c >= 0 && c < NUM_HOSTS, ESP_ERR_INVALID_ARG, TAG, "invalid host");
    struct spi_bus_t *b = &s_bus[c];
    ESP_RETURN_ON_FALSE(!b->used, ESP_ERR_INVALID_STATE, TAG, "host already in use");

    b->ctrl = c;
    b->base = SPI_BASE(c);
    b->lock = xSemaphoreCreateMutex();
    b->ndev = 0;

    F101_REG32(SPI_BGR) &= ~(1u << (16 + c));
    F101_REG32(SPI_BGR) |= (1u << c);
    F101_REG32(SPI_BGR) |= (1u << (16 + c));

    for (int i = 0; i < 5; i++) {
        int pin = cfg->iocfg[i];

        if (pin >= 0) {
            f101_pin_mux(pin, 3);
            f101_pin_drive(pin, 2);
        }
    }
    wr(b, SPI_GC, GC_EN | GC_MODE | GC_TP_EN | (1u << 31));          /* soft reset, enable, master */
    for (int i = 0; i < 1000 && (rd(b, SPI_GC) & (1u << 31)); i++) {
        f101_delay_us(1);
    }
    wr(b, SPI_GC, GC_EN | GC_MODE | GC_TP_EN);
    set_clock(b, 1000000);
    b->used = true;
    return ESP_OK;
}

esp_err_t spi_bus_free(spi_host_device_t host_id)
{
    int c = CTRL_OF(host_id);
    ESP_RETURN_ON_FALSE(c >= 0 && c < NUM_HOSTS && s_bus[c].used, ESP_ERR_INVALID_STATE, TAG, "host not initialized");
    ESP_RETURN_ON_FALSE(s_bus[c].ndev == 0, ESP_ERR_INVALID_STATE, TAG, "devices still attached");
    wr(&s_bus[c], SPI_GC, 0);
    F101_REG32(SPI_BGR) &= ~((1u << c) | (1u << (16 + c)));
    vSemaphoreDelete(s_bus[c].lock);
    s_bus[c].used = false;
    return ESP_OK;
}

esp_err_t spi_bus_get_max_transaction_len(spi_host_device_t host_id, size_t *max_bytes)
{
    ESP_RETURN_ON_FALSE(max_bytes, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    (void)host_id;
    *max_bytes = MAX_BURST;
    return ESP_OK;
}

esp_err_t spi_bus_add_device(spi_host_device_t host_id, const spi_device_interface_config_t *cfg, spi_device_handle_t *ret)
{
    ESP_RETURN_ON_FALSE(cfg && ret, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    int c = CTRL_OF(host_id);
    ESP_RETURN_ON_FALSE(c >= 0 && c < NUM_HOSTS && s_bus[c].used, ESP_ERR_INVALID_STATE, TAG, "host not initialized");
    ESP_RETURN_ON_FALSE(cfg->command_bits % 8 == 0 && cfg->address_bits % 8 == 0 && cfg->dummy_bits % 8 == 0,
                        ESP_ERR_NOT_SUPPORTED, TAG, "command, address and dummy phases must be whole bytes");
    ESP_RETURN_ON_FALSE(cfg->mode <= 3, ESP_ERR_INVALID_ARG, TAG, "invalid mode");
    struct spi_device_t *d = calloc(1, sizeof(*d));
    ESP_RETURN_ON_FALSE(d, ESP_ERR_NO_MEM, TAG, "no memory");
    d->bus = &s_bus[c];
    d->cfg = *cfg;
    d->actual_hz = cfg->clock_speed_hz > 0 ? cfg->clock_speed_hz : 1000000;
    if (cfg->spics_io_num >= 0) {
        if (cfg->spics_io_num == NATIVE_CS_PIN && c == 0) {
            f101_pin_mux(cfg->spics_io_num, 3);
            f101_pin_pull(cfg->spics_io_num, F101_PULL_UP);
        } else {
            f101_gpio_set_level(cfg->spics_io_num, !(cfg->flags & SPI_DEVICE_POSITIVE_CS));
            f101_pin_mux(cfg->spics_io_num, F101_GPIO_OUT);
        }
    }
    s_bus[c].ndev++;
    *ret = d;
    return ESP_OK;
}

esp_err_t spi_bus_remove_device(spi_device_handle_t h)
{
    ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    h->bus->ndev--;
    free(h);
    return ESP_OK;
}

esp_err_t spi_device_get_actual_freq(spi_device_handle_t h, int *freq_khz)
{
    ESP_RETURN_ON_FALSE(h && freq_khz, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    *freq_khz = (int)(h->actual_hz / 1000);
    return ESP_OK;
}

esp_err_t spi_device_acquire_bus(spi_device_handle_t h, uint32_t wait)
{
    ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    return xSemaphoreTake(h->bus->lock, wait) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

void spi_device_release_bus(spi_device_handle_t h)
{
    xSemaphoreGive(h->bus->lock);
}

static bool uses_native_cs(struct spi_device_t *d)
{
    return d->cfg.spics_io_num < 0 || (d->cfg.spics_io_num == NATIVE_CS_PIN && d->bus->ctrl == 0);
}

/* one transaction, bus lock held */
static esp_err_t transfer(struct spi_device_t *d, spi_transaction_t *t)
{
    struct spi_bus_t *b = d->bus;
    uint8_t prefix[32];
    size_t pn = 0;
    uint32_t hz = t->override_freq_hz ? t->override_freq_hz : (uint32_t)d->cfg.clock_speed_hz;
    uint8_t cmd_bits = (t->flags & SPI_TRANS_VARIABLE_CMD) ? 0 : d->cfg.command_bits;
    uint8_t addr_bits = (t->flags & SPI_TRANS_VARIABLE_ADDR) ? 0 : d->cfg.address_bits;

    for (int i = cmd_bits / 8 - 1; i >= 0; i--) {
        prefix[pn++] = (uint8_t)(t->cmd >> (8 * i));
    }
    for (int i = addr_bits / 8 - 1; i >= 0; i--) {
        prefix[pn++] = (uint8_t)(t->addr >> (8 * i));
    }
    for (int i = 0; i < d->cfg.dummy_bits / 8; i++) {
        prefix[pn++] = 0;
    }

    const uint8_t *txd = NULL;
    uint8_t *rxd = NULL;
    size_t tx_data = t->length / 8;
    size_t rx_data = 0;

    if (t->flags & SPI_TRANS_USE_TXDATA) {
        txd = t->tx_data;
        tx_data = t->length / 8;
    } else if (t->tx_buffer) {
        txd = t->tx_buffer;
    } else {
        tx_data = 0;
    }
    if (t->flags & SPI_TRANS_USE_RXDATA) {
        rxd = t->rx_data;
        rx_data = (t->rxlength ? t->rxlength : t->length) / 8;
    } else if (t->rx_buffer) {
        rxd = t->rx_buffer;
        rx_data = (t->rxlength ? t->rxlength : t->length) / 8;
    }
    ESP_RETURN_ON_FALSE(t->length % 8 == 0 && t->rxlength % 8 == 0, ESP_ERR_NOT_SUPPORTED, TAG, "whole bytes only");

    /* the bytes the master clocks out and the bytes it keeps */
    size_t tx_len = pn + tx_data;
    size_t rx_len = rx_data;
    bool full_duplex = pn == 0 && txd && rxd;
    size_t burst;

    if (full_duplex) {
        rx_len = tx_len = tx_data < rx_data ? tx_data : rx_data;
        burst = tx_len;
    } else {
        burst = tx_len + rx_len;
    }
    ESP_RETURN_ON_FALSE(burst > 0 && burst <= MAX_BURST, ESP_ERR_INVALID_ARG, TAG, "invalid transaction length");

    d->actual_hz = set_clock(b, hz ? hz : 1000000);

    uint32_t tc = TC_DDB | TC_SDM;
    if (d->cfg.mode & 1) tc |= TC_CPHA;
    if (d->cfg.mode & 2) tc |= TC_CPOL;
    if (!(d->cfg.flags & SPI_DEVICE_POSITIVE_CS)) tc |= TC_SPOL;
    if (d->cfg.flags & SPI_DEVICE_BIT_LSBFIRST) tc |= TC_FBS;
    bool native = uses_native_cs(d);
    tc |= native ? TC_SS_LEVEL : (TC_SS_OWNER | TC_SS_LEVEL);
    wr(b, SPI_GC, rd(b, SPI_GC) | GC_EN | GC_MODE | GC_TP_EN);
    wr(b, SPI_TC, tc);
    wr(b, SPI_SDC, 0);
    wr(b, SPI_INT_CTL, 0);
    wr(b, SPI_INT_STA, 0xffffffffu);
    ESP_RETURN_ON_ERROR(reset_fifo(b), TAG, "fifo reset timed out");

    if (!full_duplex) {
        wr(b, SPI_TC, rd(b, SPI_TC) | TC_DHB);      /* drop the bytes received while the command goes out */
    }
    wr(b, SPI_BURST_CNT, burst);
    wr(b, SPI_TX_CNT, tx_len);
    wr(b, SPI_BCC, (rd(b, SPI_BCC) & ~0x3fffffffu) | (full_duplex ? burst : tx_len));

    if (!native) {
        f101_gpio_set_level(d->cfg.spics_io_num, !!(d->cfg.flags & SPI_DEVICE_POSITIVE_CS));
    }

    size_t sent = 0, got = 0;
    bool started = false;
    uint64_t end = f101_mtime_get() + 2000ull * 1000 * (F101_MTIME_HZ / 1000000u);
    esp_err_t e = ESP_OK;

    for (;;) {
        /* keep the transmit fifo full */
        uint32_t cnt = (rd(b, SPI_FIFO_STA) >> 16) & 0xff;
        while (sent < tx_len && cnt < FIFO_DEPTH) {
            uint8_t v = sent < pn ? prefix[sent] : (txd ? txd[sent - pn] : 0);
            *(volatile uint8_t *)(uintptr_t)(b->base + SPI_TXDATA) = v;
            sent++;
            cnt++;
        }
        if (!started) {
            wr(b, SPI_TC, rd(b, SPI_TC) | TC_XCH);
            started = true;
        }
        while ((cnt = rd(b, SPI_FIFO_STA) & 0xff) != 0) {
            uint8_t v = *(volatile uint8_t *)(uintptr_t)(b->base + SPI_RXDATA);
            if (rxd && got < rx_len) {
                rxd[got] = v;
            }
            got++;
        }
        if (rd(b, SPI_INT_STA) & INT_ERR) {
            e = ESP_FAIL;
            break;
        }
        if (sent == tx_len && (rd(b, SPI_INT_STA) & INT_TC) && (rd(b, SPI_FIFO_STA) & 0xff) == 0) {
            break;
        }
        if (f101_mtime_get() > end) {
            e = ESP_ERR_TIMEOUT;
            break;
        }
    }
    wr(b, SPI_INT_STA, 0xffffffffu);
    wr(b, SPI_TC, rd(b, SPI_TC) | TC_SS_LEVEL);
    if (!native) {
        f101_gpio_set_level(d->cfg.spics_io_num, !(d->cfg.flags & SPI_DEVICE_POSITIVE_CS));
    }
    if (e == ESP_OK && got != rx_len) {
        e = ESP_FAIL;
    }
    return e;
}

esp_err_t spi_device_polling_transmit(spi_device_handle_t h, spi_transaction_t *t)
{
    ESP_RETURN_ON_FALSE(h && t, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    xSemaphoreTake(h->bus->lock, portMAX_DELAY);
    esp_err_t e = transfer(h, t);
    xSemaphoreGive(h->bus->lock);
    if (h->cfg.post_cb) {
        h->cfg.post_cb(t);
    }
    return e;
}

esp_err_t spi_device_transmit(spi_device_handle_t h, spi_transaction_t *t)
{
    return spi_device_polling_transmit(h, t);
}
