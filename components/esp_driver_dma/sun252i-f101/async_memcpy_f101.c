/*
 * SPDX-FileCopyrightText: 2026 Yuzuki Tsuru
 * SPDX-License-Identifier: Apache-2.0
 *
 * esp_async_memcpy() on one memory to memory DMA channel. Requests are queued (up to `backlog`)
 * and run one after the other; the callback runs in the interrupt of the DMA. The caches are
 * maintained here: the source is written back before a copy and the destination is invalidated
 * before it starts and after it ended.
 */

#include <string.h>
#include <stdlib.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_cache.h"
#include "esp_async_memcpy.h"
#include "esp_private/dma_f101.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "async_memcpy";

typedef struct {
    void *dst;
    void *src;
    size_t n;
    async_memcpy_isr_cb_t cb;
    void *cb_args;
} request_t;

struct async_memcpy_context_t {
    f101_dma_handle_t chan;
    request_t *queue;
    uint32_t backlog;
    uint32_t head;          /* the request that is running or the next one */
    uint32_t count;         /* requests in the queue, the running one included */
    bool running;
    portMUX_TYPE lock;
};

static bool memcpy_event(f101_dma_handle_t chan, f101_dma_event_t ev, void *arg);

static void start_next(struct async_memcpy_context_t *mcp)
{
    request_t *r = &mcp->queue[mcp->head];
    bool word = ((uintptr_t)r->dst | (uintptr_t)r->src | r->n) % 4 == 0;
    f101_dma_config_t cfg = {
        .dir = F101_DMA_M2M,
        .width = word ? 4 : 1,
        .burst = word ? 4 : 1,
        .cb = memcpy_event,
        .cb_arg = mcp,
    };
    f101_dma_block_t blk = { .src = (uint32_t)(uintptr_t)r->src, .dst = (uint32_t)(uintptr_t)r->dst, .len = (uint32_t)r->n };

    esp_cache_msync(r->src, r->n, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    esp_cache_msync(r->dst, r->n, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE | ESP_CACHE_MSYNC_FLAG_TYPE_DATA | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    mcp->running = true;
    f101_dma_start(mcp->chan, &cfg, &blk, 1);
}

static bool memcpy_event(f101_dma_handle_t chan, f101_dma_event_t ev, void *arg)
{
    struct async_memcpy_context_t *mcp = arg;
    request_t done = mcp->queue[mcp->head];
    bool woken = false;

    esp_cache_msync(done.dst, done.n, ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_DATA | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    mcp->head = (mcp->head + 1) % mcp->backlog;
    mcp->count--;
    mcp->running = false;
    if (mcp->count) {
        start_next(mcp);
    }
    if (done.cb) {
        async_memcpy_event_t event = { .data = NULL };
        woken = done.cb(mcp, &event, done.cb_args);
    }
    return woken;
}

esp_err_t esp_async_memcpy_install(const async_memcpy_config_t *config, async_memcpy_handle_t *mcp)
{
    ESP_RETURN_ON_FALSE(config && mcp, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(config->backlog > 0, ESP_ERR_INVALID_ARG, TAG, "backlog must be positive");

    struct async_memcpy_context_t *c = calloc(1, sizeof(*c));
    ESP_RETURN_ON_FALSE(c, ESP_ERR_NO_MEM, TAG, "no memory");
    c->queue = calloc(config->backlog, sizeof(request_t));
    if (!c->queue) {
        free(c);
        return ESP_ERR_NO_MEM;
    }
    c->backlog = config->backlog;
    portMUX_INITIALIZE(&c->lock);
    esp_err_t err = f101_dma_new_channel(&c->chan);
    if (err != ESP_OK) {
        free(c->queue);
        free(c);
        return err;
    }
    *mcp = c;
    return ESP_OK;
}

esp_err_t esp_async_memcpy_install_gdma_ahb(const async_memcpy_config_t *config, async_memcpy_handle_t *mcp)
{
    return esp_async_memcpy_install(config, mcp);
}

esp_err_t esp_async_memcpy_uninstall(async_memcpy_handle_t mcp)
{
    ESP_RETURN_ON_FALSE(mcp, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(mcp->count == 0, ESP_ERR_INVALID_STATE, TAG, "copies are still pending");
    f101_dma_del_channel(mcp->chan);
    free(mcp->queue);
    free(mcp);
    return ESP_OK;
}

esp_err_t esp_async_memcpy(async_memcpy_handle_t mcp, void *dst, void *src, size_t n, async_memcpy_isr_cb_t cb_isr, void *cb_args)
{
    ESP_RETURN_ON_FALSE(mcp && dst && src && n > 0 && n <= 0xffffff, ESP_ERR_INVALID_ARG, TAG, "invalid argument");

    esp_err_t err = ESP_OK;

    portENTER_CRITICAL_SAFE(&mcp->lock);
    if (mcp->count == mcp->backlog) {
        err = ESP_ERR_NO_MEM;
    } else {
        request_t *r = &mcp->queue[(mcp->head + mcp->count) % mcp->backlog];

        r->dst = dst;
        r->src = src;
        r->n = n;
        r->cb = cb_isr;
        r->cb_args = cb_args;
        mcp->count++;
        if (!mcp->running) {
            start_next(mcp);
        }
    }
    portEXIT_CRITICAL_SAFE(&mcp->lock);
    return err;
}
