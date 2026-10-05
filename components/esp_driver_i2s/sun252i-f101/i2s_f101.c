/*
 * SPDX-FileCopyrightText: 2026 Yuzuki Tsuru
 * SPDX-License-Identifier: Apache-2.0
 *
 * driver/i2s_std.h on the on-chip audio codec (I2S_NUM_0): the DAC is the TX channel, the ADC the RX
 * channel. A channel owns a ring of dma_desc_num periods of dma_frame_num frames that a cyclic DMA
 * chain moves to or from the codec FIFO; every period raises an event. TX periods are zeroed once
 * they were played, so a stream that is not kept fed plays silence. RX periods are handed to the
 * reader a period late: the end of a period is reported while its last words can still be on their
 * way to memory.
 */

#include <string.h>
#include <stdlib.h>
#include "sdkconfig.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_private/dma_f101.h"
#include "codec_f101_priv.h"

static const char *TAG = "i2s";

#define MAX_PERIODS     32

typedef enum {
    CHAN_INIT,          /*!< allocated, not configured */
    CHAN_READY,         /*!< configured, disabled */
    CHAN_RUNNING,
} chan_state_t;

struct i2s_channel_obj_t {
    int id;
    i2s_dir_t dir;
    i2s_role_t role;
    chan_state_t state;
    i2s_chan_handle_t pair;
    i2s_std_config_t std_cfg;
    uint32_t desc_num;
    uint32_t frame_num;
    uint32_t sample_rate;
    uint8_t data_bytes;                 /* 2 or 4 */
    uint8_t channels;                   /* 1 or 2 */
    uint32_t frame_bytes;
    size_t period_size;                 /* bytes of one period */
    size_t stride;                      /* distance of two periods in the ring, a multiple of the cache line */
    uint8_t *ring;
    f101_dma_handle_t dma;
    bool dma_started;
    i2s_event_callbacks_t cbs;
    void *cb_ctx;
    bool auto_clear_after_cb;
    bool auto_clear_before_cb;

    /* TX: the writer fills periods in ring order; RX: the reader empties them in ring order */
    volatile uint8_t full[MAX_PERIODS]; /* TX: the period holds data that was not played yet; RX: the period holds data that was not read yet */
    uint32_t cur;                       /* TX: period being filled; RX: period being read */
    size_t cur_off;                     /* bytes done in `cur` */
    uint32_t dma_idx;                   /* period the DMA works on */
    uint32_t dma_blocks;                /* periods completed since the start */
    SemaphoreHandle_t wake;             /* the ISR gives it when a period was played or arrived */
    portMUX_TYPE lock;
    i2s_chan_info_t info;
};

static i2s_chan_handle_t s_chan[2];     /* [0] TX, [1] RX of I2S_NUM_0 */

static inline uint8_t *period_ptr(i2s_chan_handle_t c, uint32_t idx)
{
    return c->ring + (size_t)idx * c->stride;
}

static inline codec_dir_t codec_dir(i2s_chan_handle_t c)
{
    return c->dir == I2S_DIR_TX ? CODEC_DIR_TX : CODEC_DIR_RX;
}

static void clean(void *p, size_t n)
{
    esp_cache_msync(p, n, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

static bool dma_event(f101_dma_handle_t dma, f101_dma_event_t ev, void *arg)
{
    i2s_chan_handle_t c = arg;
    bool woken = false;
    BaseType_t w = pdFALSE;

    if (ev != F101_DMA_EVENT_BLOCK && ev != F101_DMA_EVENT_DONE) {
        return false;                   /* errors only stop the channel, the user sees silence or no data */
    }
    uint32_t idx = c->dma_idx;

    c->dma_idx = (idx + 1) % c->desc_num;
    c->dma_blocks++;

    if (c->dir == I2S_DIR_TX) {
        uint8_t *p = period_ptr(c, idx);
        bool had_data = c->full[idx];
        i2s_event_data_t ed = { .dma_buf = p, .size = c->period_size };

        if (!had_data && c->cbs.on_send_q_ovf) {
            woken |= c->cbs.on_send_q_ovf(c, &ed, c->cb_ctx);
        }
        if (c->cbs.on_sent) {
            woken |= c->cbs.on_sent(c, &ed, c->cb_ctx);
        }
        /* what was played must not be played again when the ring comes around */
        memset(p, 0, c->period_size);
        clean(p, c->stride);
        c->full[idx] = 0;
        if (had_data) {
            xSemaphoreGiveFromISR(c->wake, &w);
            woken |= w == pdTRUE;
        }
    } else {
        if (c->dma_blocks < 2) {
            return false;               /* the first period is reported complete while it is not */
        }
        uint32_t done = (idx + c->desc_num - 1) % c->desc_num;
        uint8_t *p = period_ptr(c, done);
        i2s_event_data_t ed = { .dma_buf = p, .size = c->period_size };

        esp_cache_msync(p, c->stride, ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
        if (c->full[done]) {
            /* the reader did not keep up: the unread data is lost */
            if (c->cbs.on_recv_q_ovf) {
                woken |= c->cbs.on_recv_q_ovf(c, &ed, c->cb_ctx);
            }
            if (c->cur == done) {
                c->cur = (done + 1) % c->desc_num;
                c->cur_off = 0;
            }
        }
        c->full[done] = 1;
        if (c->cbs.on_recv) {
            woken |= c->cbs.on_recv(c, &ed, c->cb_ctx);
        }
        xSemaphoreGiveFromISR(c->wake, &w);
        woken |= w == pdTRUE;
    }
    return woken;
}

/* ---- channel management ---- */

esp_err_t i2s_new_channel(const i2s_chan_config_t *chan_cfg, i2s_chan_handle_t *ret_tx_handle, i2s_chan_handle_t *ret_rx_handle)
{
    ESP_RETURN_ON_FALSE(chan_cfg && (ret_tx_handle || ret_rx_handle), ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(chan_cfg->id == I2S_NUM_0 || chan_cfg->id == I2S_NUM_AUTO, ESP_ERR_NOT_FOUND, TAG, "only I2S_NUM_0 (the on-chip codec) exists");
    ESP_RETURN_ON_FALSE(chan_cfg->role == I2S_ROLE_MASTER, ESP_ERR_NOT_SUPPORTED, TAG, "the codec is a master only");
    ESP_RETURN_ON_FALSE(chan_cfg->dma_desc_num >= 2 && chan_cfg->dma_desc_num <= MAX_PERIODS, ESP_ERR_INVALID_ARG, TAG, "dma_desc_num is 2..%d", MAX_PERIODS);
    ESP_RETURN_ON_FALSE(chan_cfg->dma_frame_num >= 8 && chan_cfg->dma_frame_num <= 4092, ESP_ERR_INVALID_ARG, TAG, "dma_frame_num is 8..4092");
    ESP_RETURN_ON_FALSE((!ret_tx_handle || !s_chan[0]) && (!ret_rx_handle || !s_chan[1]), ESP_ERR_NOT_FOUND, TAG, "the channel is in use");
    ESP_RETURN_ON_FALSE(chan_cfg->tx_destination == I2S_DESTINATION_DMA && chan_cfg->rx_destination == I2S_DESTINATION_DMA, ESP_ERR_NOT_SUPPORTED, TAG, "DMA data path only");
    ESP_RETURN_ON_ERROR(codec_init(), TAG, "codec");

    for (int d = 0; d < 2; d++) {
        i2s_chan_handle_t *ret = d == 0 ? ret_tx_handle : ret_rx_handle;

        if (!ret) {
            continue;
        }
        i2s_chan_handle_t c = calloc(1, sizeof(*c));

        if (!c) {
            if (d == 1 && ret_tx_handle) {
                i2s_del_channel(*ret_tx_handle);
                *ret_tx_handle = NULL;
            }
            return ESP_ERR_NO_MEM;
        }
        c->id = I2S_NUM_0;
        c->dir = d == 0 ? I2S_DIR_TX : I2S_DIR_RX;
        c->role = chan_cfg->role;
        c->desc_num = chan_cfg->dma_desc_num;
        c->frame_num = chan_cfg->dma_frame_num;
        c->auto_clear_after_cb = chan_cfg->auto_clear_after_cb;
        c->auto_clear_before_cb = chan_cfg->auto_clear_before_cb;
        c->wake = xSemaphoreCreateCounting(MAX_PERIODS, 0);
        portMUX_INITIALIZE(&c->lock);
        c->state = CHAN_INIT;
        if (!c->wake || f101_dma_new_channel(&c->dma) != ESP_OK) {
            if (c->wake) {
                vSemaphoreDelete(c->wake);
            }
            free(c);
            if (d == 1 && ret_tx_handle) {
                i2s_del_channel(*ret_tx_handle);
                *ret_tx_handle = NULL;
            }
            return ESP_ERR_NO_MEM;
        }
        s_chan[d] = c;
        *ret = c;
    }
    if (ret_tx_handle && ret_rx_handle) {
        (*ret_tx_handle)->pair = *ret_rx_handle;
        (*ret_rx_handle)->pair = *ret_tx_handle;
    }
    return ESP_OK;
}

esp_err_t i2s_del_channel(i2s_chan_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "null handle");
    ESP_RETURN_ON_FALSE(handle->state != CHAN_RUNNING, ESP_ERR_INVALID_STATE, TAG, "disable the channel first");

    if (handle->state == CHAN_READY) {
        codec_release(codec_dir(handle));
    }
    f101_dma_del_channel(handle->dma);
    if (handle->pair) {
        handle->pair->pair = NULL;
    }
    s_chan[handle->dir == I2S_DIR_TX ? 0 : 1] = NULL;
    free(handle->ring);
    vSemaphoreDelete(handle->wake);
    free(handle);
    return ESP_OK;
}

static esp_err_t apply_std(i2s_chan_handle_t c, const i2s_std_config_t *cfg)
{
    uint32_t width = cfg->slot_cfg.data_bit_width;
    uint8_t bytes;

    ESP_RETURN_ON_FALSE(width == I2S_DATA_BIT_WIDTH_16BIT || width == I2S_DATA_BIT_WIDTH_24BIT || width == I2S_DATA_BIT_WIDTH_32BIT,
                        ESP_ERR_INVALID_ARG, TAG, "16, 24 or 32 bit samples");
    bytes = width == I2S_DATA_BIT_WIDTH_16BIT ? 2 : 4;
    uint8_t channels = cfg->slot_cfg.slot_mode == I2S_SLOT_MODE_MONO ? 1 : 2;

    ESP_RETURN_ON_FALSE(c->dir == I2S_DIR_TX || channels == 1, ESP_ERR_NOT_SUPPORTED, TAG, "the ADC delivers a mono stream");
    ESP_RETURN_ON_ERROR(codec_configure(codec_dir(c), cfg->clk_cfg.sample_rate_hz, bytes, channels), TAG, "codec configuration");

    c->sample_rate = cfg->clk_cfg.sample_rate_hz;
    c->data_bytes = bytes;
    c->channels = channels;
    c->frame_bytes = bytes * channels;
    c->period_size = (size_t)c->frame_num * c->frame_bytes;
    c->stride = (c->period_size + 63u) & ~63u;
    c->std_cfg = *cfg;

    free(c->ring);
    c->ring = heap_caps_aligned_calloc(64, c->desc_num, c->stride, MALLOC_CAP_DEFAULT);
    if (!c->ring) {
        codec_release(codec_dir(c));
        return ESP_ERR_NO_MEM;
    }
    esp_cache_msync(c->ring, c->desc_num * c->stride, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
    memset((void *)c->full, 0, sizeof(c->full));
    c->cur = 0;
    c->cur_off = 0;
    return ESP_OK;
}

esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t handle, const i2s_std_config_t *std_cfg)
{
    ESP_RETURN_ON_FALSE(handle && std_cfg, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(handle->state == CHAN_INIT, ESP_ERR_INVALID_STATE, TAG, "the channel is already initialised");
    ESP_RETURN_ON_ERROR(apply_std(handle, std_cfg), TAG, "configuration");
    handle->state = CHAN_READY;
    return ESP_OK;
}

esp_err_t i2s_channel_reconfig_std_clock(i2s_chan_handle_t handle, const i2s_std_clk_config_t *clk_cfg)
{
    ESP_RETURN_ON_FALSE(handle && clk_cfg, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(handle->state == CHAN_READY, ESP_ERR_INVALID_STATE, TAG, "the channel must be initialised and disabled");
    i2s_std_config_t cfg = handle->std_cfg;

    cfg.clk_cfg = *clk_cfg;
    return apply_std(handle, &cfg);
}

esp_err_t i2s_channel_reconfig_std_slot(i2s_chan_handle_t handle, const i2s_std_slot_config_t *slot_cfg)
{
    ESP_RETURN_ON_FALSE(handle && slot_cfg, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(handle->state == CHAN_READY, ESP_ERR_INVALID_STATE, TAG, "the channel must be initialised and disabled");
    i2s_std_config_t cfg = handle->std_cfg;

    cfg.slot_cfg = *slot_cfg;
    return apply_std(handle, &cfg);
}

esp_err_t i2s_channel_reconfig_std_gpio(i2s_chan_handle_t handle, const i2s_std_gpio_config_t *gpio_cfg)
{
    ESP_RETURN_ON_FALSE(handle && gpio_cfg, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    return ESP_ERR_NOT_SUPPORTED;       /* the codec is internal: no pins */
}

esp_err_t i2s_channel_get_info(i2s_chan_handle_t handle, i2s_chan_info_t *chan_info)
{
    ESP_RETURN_ON_FALSE(handle && chan_info, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    memset(chan_info, 0, sizeof(*chan_info));
    chan_info->id = handle->id;
    chan_info->role = handle->role;
    chan_info->dir = handle->dir;
    chan_info->mode = I2S_COMM_MODE_STD;
    chan_info->is_enabled = handle->state == CHAN_RUNNING;
    chan_info->pair_chan = handle->pair;
    chan_info->total_dma_buf_size = handle->ring ? handle->desc_num * handle->period_size : 0;
    chan_info->sclk_hz = handle->sample_rate * 256;
    chan_info->mclk_hz = handle->sample_rate * 256;
    chan_info->bclk_hz = handle->sample_rate * handle->frame_bytes * 8;
    chan_info->mode_cfg = &handle->std_cfg;
    return ESP_OK;
}

esp_err_t i2s_channel_register_event_callback(i2s_chan_handle_t handle, const i2s_event_callbacks_t *callbacks, void *user_data)
{
    ESP_RETURN_ON_FALSE(handle && callbacks, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(handle->state != CHAN_RUNNING, ESP_ERR_INVALID_STATE, TAG, "disable the channel first");
    if (handle->dir == I2S_DIR_TX) {
        ESP_RETURN_ON_FALSE(!callbacks->on_recv && !callbacks->on_recv_q_ovf, ESP_ERR_INVALID_ARG, TAG, "receive events on a TX channel");
    } else {
        ESP_RETURN_ON_FALSE(!callbacks->on_sent && !callbacks->on_send_q_ovf, ESP_ERR_INVALID_ARG, TAG, "send events on an RX channel");
    }
    handle->cbs = *callbacks;
    handle->cb_ctx = user_data;
    return ESP_OK;
}

/* ---- data path ---- */

/* copy as much as fits into the free periods; blocks up to ticks when `ticks` is not zero */
static esp_err_t tx_push(i2s_chan_handle_t c, const uint8_t *src, size_t size, size_t *done, TickType_t ticks)
{
    TickType_t deadline_start = xTaskGetTickCount();
    size_t total = 0;
    esp_err_t err = ESP_OK;

    while (total < size) {
        bool can_fill;

        portENTER_CRITICAL(&c->lock);
        can_fill = !c->full[c->cur];
        portEXIT_CRITICAL(&c->lock);
        if (!can_fill) {
            /* the ring is full: wait for a period to be played */
            TickType_t waited = xTaskGetTickCount() - deadline_start;
            TickType_t left = ticks == portMAX_DELAY ? portMAX_DELAY : (waited >= ticks ? 0 : ticks - waited);

            if (!left || xSemaphoreTake(c->wake, left) != pdTRUE) {
                err = ESP_ERR_TIMEOUT;
                break;
            }
            continue;
        }
        uint8_t *p = period_ptr(c, c->cur);
        size_t n = c->period_size - c->cur_off;

        if (n > size - total) {
            n = size - total;
        }
        memcpy(p + c->cur_off, src + total, n);
        clean(p + c->cur_off, n);
        c->cur_off += n;
        total += n;
        if (c->cur_off == c->period_size) {
            portENTER_CRITICAL(&c->lock);
            c->full[c->cur] = 1;
            portEXIT_CRITICAL(&c->lock);
            c->cur = (c->cur + 1) % c->desc_num;
            c->cur_off = 0;
        }
    }
    *done = total;
    return err;
}

esp_err_t i2s_channel_write(i2s_chan_handle_t handle, const void *src, size_t size, size_t *bytes_written, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(handle && src, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(handle->dir == I2S_DIR_TX, ESP_ERR_INVALID_ARG, TAG, "not a TX channel");
    ESP_RETURN_ON_FALSE(handle->state == CHAN_RUNNING, ESP_ERR_INVALID_STATE, TAG, "the channel is not enabled");

    size_t done = 0;
    esp_err_t err = tx_push(handle, src, size, &done, timeout_ms == portMAX_DELAY ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms));

    if (bytes_written) {
        *bytes_written = done;
    }
    return err;
}

esp_err_t i2s_channel_preload_data(i2s_chan_handle_t tx_handle, const void *src, size_t size, size_t *bytes_loaded)
{
    ESP_RETURN_ON_FALSE(tx_handle && src, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(tx_handle->dir == I2S_DIR_TX, ESP_ERR_INVALID_ARG, TAG, "not a TX channel");
    ESP_RETURN_ON_FALSE(tx_handle->state == CHAN_READY, ESP_ERR_INVALID_STATE, TAG, "preload before the channel is enabled");

    size_t done = 0;

    tx_push(tx_handle, src, size, &done, 0);
    if (bytes_loaded) {
        *bytes_loaded = done;
    }
    return ESP_OK;
}

esp_err_t i2s_channel_read(i2s_chan_handle_t handle, void *dest, size_t size, size_t *bytes_read, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(handle && dest, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(handle->dir == I2S_DIR_RX, ESP_ERR_INVALID_ARG, TAG, "not an RX channel");
    ESP_RETURN_ON_FALSE(handle->state == CHAN_RUNNING, ESP_ERR_INVALID_STATE, TAG, "the channel is not enabled");

    TickType_t ticks = timeout_ms == portMAX_DELAY ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    TickType_t start = xTaskGetTickCount();
    size_t total = 0;
    esp_err_t err = ESP_OK;
    i2s_chan_handle_t c = handle;

    while (total < size) {
        bool avail;

        portENTER_CRITICAL(&c->lock);
        avail = c->full[c->cur];
        portEXIT_CRITICAL(&c->lock);
        if (!avail) {
            TickType_t waited = xTaskGetTickCount() - start;
            TickType_t left = ticks == portMAX_DELAY ? portMAX_DELAY : (waited >= ticks ? 0 : ticks - waited);

            if (!left || xSemaphoreTake(c->wake, left) != pdTRUE) {
                err = ESP_ERR_TIMEOUT;
                break;
            }
            continue;
        }
        size_t n = c->period_size - c->cur_off;

        if (n > size - total) {
            n = size - total;
        }
        memcpy((uint8_t *)dest + total, period_ptr(c, c->cur) + c->cur_off, n);
        c->cur_off += n;
        total += n;
        if (c->cur_off == c->period_size) {
            portENTER_CRITICAL(&c->lock);
            c->full[c->cur] = 0;
            portEXIT_CRITICAL(&c->lock);
            c->cur = (c->cur + 1) % c->desc_num;
            c->cur_off = 0;
        }
    }
    if (bytes_read) {
        *bytes_read = total;
    }
    return err;
}

/* ---- start and stop ---- */

esp_err_t i2s_channel_enable(i2s_chan_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "null handle");
    ESP_RETURN_ON_FALSE(handle->state == CHAN_READY, ESP_ERR_INVALID_STATE, TAG, "the channel is not initialised or already enabled");

    bool tx = handle->dir == I2S_DIR_TX;
    f101_dma_block_t blocks[MAX_PERIODS];

    for (uint32_t i = 0; i < handle->desc_num; i++) {
        uint32_t mem = (uint32_t)(uintptr_t)period_ptr(handle, i);

        blocks[i].len = (uint32_t)handle->period_size;
        blocks[i].src = tx ? mem : codec_fifo_addr(CODEC_DIR_RX);
        blocks[i].dst = tx ? codec_fifo_addr(CODEC_DIR_TX) : mem;
    }
    if (!tx) {
        memset(handle->ring, 0, handle->desc_num * handle->stride);
        esp_cache_msync(handle->ring, handle->desc_num * handle->stride, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
        memset((void *)handle->full, 0, sizeof(handle->full));
        handle->cur = 0;
        handle->cur_off = 0;
    }
    while (xSemaphoreTake(handle->wake, 0) == pdTRUE) {
    }
    handle->dma_idx = 0;
    handle->dma_blocks = 0;

    f101_dma_config_t cfg = {
        .dir = tx ? F101_DMA_M2P : F101_DMA_P2M,
        .slot = CODEC_DMA_SLOT,
        .width = handle->data_bytes,
        .burst = 4,
        .cyclic = true,
        .block_events = true,
        .cb = dma_event,
        .cb_arg = handle,
    };

    handle->state = CHAN_RUNNING;
    esp_err_t err = f101_dma_start(handle->dma, &cfg, blocks, handle->desc_num);

    if (err != ESP_OK) {
        handle->state = CHAN_READY;
        return err;
    }
    codec_start(codec_dir(handle));
    return ESP_OK;
}

esp_err_t i2s_channel_disable(i2s_chan_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "null handle");
    ESP_RETURN_ON_FALSE(handle->state == CHAN_RUNNING, ESP_ERR_INVALID_STATE, TAG, "the channel is not enabled");

    codec_stop(codec_dir(handle));
    f101_dma_stop(handle->dma);
    handle->state = CHAN_READY;
    /* data that was not played is dropped, the ring is silent again */
    if (handle->dir == I2S_DIR_TX) {
        memset(handle->ring, 0, handle->desc_num * handle->stride);
        esp_cache_msync(handle->ring, handle->desc_num * handle->stride, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
        memset((void *)handle->full, 0, sizeof(handle->full));
        handle->cur = 0;
        handle->cur_off = 0;
    }
    return ESP_OK;
}

esp_err_t i2s_channel_tune_rate(i2s_chan_handle_t handle, const i2s_tuning_config_t *tune_cfg, i2s_tuning_info_t *tune_info)
{
    return ESP_ERR_NOT_SUPPORTED;
}
