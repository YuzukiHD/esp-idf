/*
 * SPDX-FileCopyrightText: 2026 Yuzuki Tsuru
 * SPDX-License-Identifier: Apache-2.0
 *
 * DMA controller at 0x03002000: bus gate, reset and MBUS gate in the CCU, interrupt 50 of the PLIC,
 * 12 channels that follow a chain of 24 byte descriptors in memory.
 */

#include <string.h>
#include <stdlib.h>
#include "sdkconfig.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_intr_alloc.h"
#include "esp_cache.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/sun252i_f101_ll.h"
#include "esp_private/dma_f101.h"

static const char *TAG = "dma";

#define DMA_BASE            0x03002000u
#define DMA_IRQ_SRC         50
#define DMA_CHANNELS        12
#define DMA_BUS_GATE        (F101_CCU_BASE + 0x70cu)        /* bit 0 gate, bit 16 reset */
#define DMA_MBUS_GATE       (F101_CCU_BASE + 0x804u)

#define REG_IRQ_EN(g)       (DMA_BASE + 0x00u + (g) * 4u)
#define REG_IRQ_STAT(g)     (DMA_BASE + 0x10u + (g) * 4u)
#define REG_SECURE          (DMA_BASE + 0x20u)
#define REG_GATE            (DMA_BASE + 0x28u)
#define CH_BASE(c)          (DMA_BASE + 0x100u + (c) * 0x40u)
#define CH_ENABLE           0x00u
#define CH_PAUSE            0x04u
#define CH_LLI              0x08u
#define CH_CUR_SRC          0x10u
#define CH_CUR_DST          0x14u
#define CH_CNT              0x18u

#define IRQ_PACKAGE         (1u << 1)       /* a block is done */
#define IRQ_QUEUE           (1u << 2)       /* the chain is done */
#define IRQ_TIMEOUT         (1u << 3)

#define LINK_END            0xfffff800u
#define MAX_BLOCK           0x00ffffffu
#define MAX_BLOCKS          32

#define SRC_WIDTH(n)        ((n) << 9)
#define SRC_BURST(n)        ((n) << 6)
#define SRC_IO              (1u << 8)
#define SRC_DRQ(n)          ((n) << 0)
#define DST_WIDTH(n)        ((n) << 25)
#define DST_BURST(n)        ((n) << 22)
#define DST_IO              (1u << 24)
#define DST_DRQ(n)          ((n) << 16)

typedef struct {
    uint32_t cfg;
    uint32_t src;
    uint32_t dst;
    uint32_t len;
    uint32_t para;
    uint32_t next;
    uint32_t pad[2];                /* 32 byte stride */
} dma_lli_t;

struct f101_dma_channel {
    int id;
    bool in_use;
    volatile bool busy;
    f101_dma_dir_t dir;
    f101_dma_cb_t cb;
    void *cb_arg;
    bool block_events;
    dma_lli_t *lli;
    size_t num_blocks;
    uint32_t block_src[MAX_BLOCKS];     /* memory side start of each block, for the position */
    uint32_t block_len[MAX_BLOCKS];
};

static struct f101_dma_channel s_chan[DMA_CHANNELS];
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_inited;
static intr_handle_t s_intr;

static inline uint32_t irq_shift(int c)
{
    return (c % 8) * 4u;
}

static void irq_set(int c, bool on, bool per_block)
{
    uint32_t mask = (IRQ_QUEUE | IRQ_TIMEOUT | (per_block ? IRQ_PACKAGE : 0)) << irq_shift(c);
    uint32_t reg = F101_REG32(REG_IRQ_EN(c / 8));

    F101_REG32(REG_IRQ_EN(c / 8)) = on ? (reg | mask) : (reg & ~mask);
}

static void dma_isr(void *arg)
{
    uint32_t st[2] = { F101_REG32(REG_IRQ_STAT(0)), F101_REG32(REG_IRQ_STAT(1)) };
    bool woken = false;

    F101_REG32(REG_IRQ_STAT(0)) = st[0];
    F101_REG32(REG_IRQ_STAT(1)) = st[1];
    for (int c = 0; c < DMA_CHANNELS; c++) {
        uint32_t ev = (st[c / 8] >> irq_shift(c)) & 0xf;
        struct f101_dma_channel *ch = &s_chan[c];

        if (!ev || !ch->in_use) {
            continue;
        }
        f101_dma_event_t type;
        if (ev & IRQ_TIMEOUT) {
            type = F101_DMA_EVENT_ERROR;
        } else if (!(ev & IRQ_QUEUE) && (ev & IRQ_PACKAGE)) {
            type = F101_DMA_EVENT_BLOCK;
        } else if (ev & (IRQ_QUEUE | 1u | IRQ_PACKAGE)) {
            type = F101_DMA_EVENT_DONE;
        } else {
            type = F101_DMA_EVENT_ERROR;
        }
        if (type != F101_DMA_EVENT_BLOCK) {
            ch->busy = false;
            irq_set(c, false, true);
        }
        if (ch->cb) {
            woken |= ch->cb(ch, type, ch->cb_arg);
        }
    }
    if (woken) {
        portYIELD_FROM_ISR();
    }
}

static esp_err_t dma_init_once(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    F101_REG32(DMA_BUS_GATE) |= (1u << 0) | (1u << 16);
    F101_REG32(DMA_MBUS_GATE) |= 1u;
    F101_REG32(REG_GATE) = 0x7;                                 /* no automatic clock gating */
    F101_REG32(REG_SECURE) = (1u << DMA_CHANNELS) - 1;          /* every channel non-secure */
    for (int g = 0; g < 2; g++) {
        F101_REG32(REG_IRQ_EN(g)) = 0;
        F101_REG32(REG_IRQ_STAT(g)) = 0xffffffffu;
    }
    for (int c = 0; c < DMA_CHANNELS; c++) {
        s_chan[c].id = c;
    }
    ESP_RETURN_ON_ERROR(esp_intr_alloc(DMA_IRQ_SRC, 0, dma_isr, NULL, &s_intr), TAG, "interrupt");
    s_inited = true;
    return ESP_OK;
}

esp_err_t f101_dma_new_channel(f101_dma_handle_t *ret_chan)
{
    ESP_RETURN_ON_FALSE(ret_chan, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_ERROR(dma_init_once(), TAG, "init");

    esp_err_t err = ESP_ERR_NOT_FOUND;

    portENTER_CRITICAL(&s_lock);
    for (int c = 0; c < DMA_CHANNELS; c++) {
        if (!s_chan[c].in_use) {
            memset(&s_chan[c], 0, sizeof(s_chan[c]));
            s_chan[c].id = c;
            s_chan[c].in_use = true;
            *ret_chan = &s_chan[c];
            err = ESP_OK;
            break;
        }
    }
    portEXIT_CRITICAL(&s_lock);
    return err;
}

esp_err_t f101_dma_del_channel(f101_dma_handle_t chan)
{
    ESP_RETURN_ON_FALSE(chan && chan->in_use, ESP_ERR_INVALID_ARG, TAG, "invalid channel");
    f101_dma_stop(chan);
    chan->in_use = false;
    return ESP_OK;
}

static bool width_valid(uint8_t w)
{
    return w == 1 || w == 2 || w == 4;
}

static int burst_code(uint8_t b)
{
    switch (b) {
    case 0:
    case 1:
        return 0;
    case 4:
        return 1;
    case 8:
        return 2;
    case 16:
        return 3;
    default:
        return -1;
    }
}

esp_err_t f101_dma_start(f101_dma_handle_t chan, const f101_dma_config_t *config, const f101_dma_block_t *blocks, size_t num_blocks)
{
    ESP_RETURN_ON_FALSE(chan && chan->in_use && config && blocks, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(num_blocks > 0 && num_blocks <= MAX_BLOCKS, ESP_ERR_INVALID_ARG, TAG, "1..%d blocks", MAX_BLOCKS);
    ESP_RETURN_ON_FALSE(width_valid(config->width) && burst_code(config->burst) >= 0, ESP_ERR_INVALID_ARG, TAG, "width or burst");
    ESP_RETURN_ON_FALSE(!chan->busy, ESP_ERR_INVALID_STATE, TAG, "channel is busy");

    uint32_t w = config->width == 1 ? 0 : (config->width == 2 ? 1 : 2);
    uint32_t base = SRC_WIDTH(w) | SRC_BURST(burst_code(config->burst)) | DST_WIDTH(w) | DST_BURST(burst_code(config->burst));

    switch (config->dir) {
    case F101_DMA_M2M:
        break;
    case F101_DMA_M2P:
        base |= DST_DRQ(config->slot) | DST_IO;
        break;
    case F101_DMA_P2M:
        base |= SRC_DRQ(config->slot) | SRC_IO;
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < num_blocks; i++) {
        ESP_RETURN_ON_FALSE(blocks[i].len > 0 && blocks[i].len <= MAX_BLOCK, ESP_ERR_INVALID_ARG, TAG, "block %u length", (unsigned)i);
    }
    dma_lli_t *lli = heap_caps_aligned_calloc(64, 1, (num_blocks * sizeof(dma_lli_t) + 63u) & ~63u, MALLOC_CAP_DEFAULT);
    ESP_RETURN_ON_FALSE(lli, ESP_ERR_NO_MEM, TAG, "no memory for the descriptors");

    for (size_t i = 0; i < num_blocks; i++) {
        lli[i].cfg = base;
        lli[i].src = blocks[i].src;
        lli[i].dst = blocks[i].dst;
        lli[i].len = blocks[i].len;
        lli[i].para = 64;
        if (i + 1 < num_blocks) {
            lli[i].next = (uint32_t)(uintptr_t)&lli[i + 1];
        } else {
            lli[i].next = config->cyclic ? (uint32_t)(uintptr_t)&lli[0] : LINK_END;
        }
        chan->block_src[i] = config->dir == F101_DMA_P2M ? blocks[i].dst : blocks[i].src;
        chan->block_len[i] = blocks[i].len;
    }
    esp_cache_msync(lli, (num_blocks * sizeof(dma_lli_t) + 63u) & ~63u, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);

    free(chan->lli);
    chan->lli = lli;
    chan->num_blocks = num_blocks;
    chan->dir = config->dir;
    chan->cb = config->cb;
    chan->cb_arg = config->cb_arg;
    chan->block_events = config->block_events;
    chan->busy = true;

    uint32_t ch = CH_BASE(chan->id);

    irq_set(chan->id, true, config->block_events);
    F101_REG32(ch + CH_LLI) = (uint32_t)(uintptr_t)&chan->lli[0];
    F101_REG32(ch + CH_ENABLE) = 1;
    return ESP_OK;
}

esp_err_t f101_dma_stop(f101_dma_handle_t chan)
{
    ESP_RETURN_ON_FALSE(chan && chan->in_use, ESP_ERR_INVALID_ARG, TAG, "invalid channel");
    uint32_t ch = CH_BASE(chan->id);

    F101_REG32(ch + CH_PAUSE) = 1;
    F101_REG32(ch + CH_ENABLE) = 0;
    F101_REG32(ch + CH_PAUSE) = 0;
    irq_set(chan->id, false, true);
    chan->busy = false;
    free(chan->lli);
    chan->lli = NULL;
    chan->num_blocks = 0;
    return ESP_OK;
}

bool f101_dma_is_busy(f101_dma_handle_t chan)
{
    return chan && chan->busy;
}

esp_err_t f101_dma_get_position(f101_dma_handle_t chan, uint32_t *out_offset)
{
    ESP_RETURN_ON_FALSE(chan && chan->in_use && out_offset, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    uint32_t cur = F101_REG32(CH_BASE(chan->id) + (chan->dir == F101_DMA_P2M ? CH_CUR_DST : CH_CUR_SRC));
    uint32_t off = 0;

    for (size_t i = 0; i < chan->num_blocks; i++) {
        if (cur >= chan->block_src[i] && cur - chan->block_src[i] < chan->block_len[i]) {
            *out_offset = off + (cur - chan->block_src[i]);
            return ESP_OK;
        }
        off += chan->block_len[i];
    }
    *out_offset = 0;
    return ESP_OK;
}

int f101_dma_get_channel_id(f101_dma_handle_t chan)
{
    return chan ? chan->id : -1;
}
