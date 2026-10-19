/*
 * SPDX-FileCopyrightText: 2026 Yuzuki Tsuru
 * SPDX-License-Identifier: Apache-2.0
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "sd_compat.h"

#undef malloc
#undef calloc
#undef realloc
#undef aligned_alloc

static const char *TAG = "aic_sd";

static int neg(esp_err_t e)
{
    switch (e) {
    case ESP_OK:
        return 0;
    case ESP_ERR_TIMEOUT:
        return -ETIMEDOUT;
    case ESP_ERR_INVALID_ARG:
    case ESP_ERR_INVALID_SIZE:
        return -EINVAL;
    case ESP_ERR_NOT_FOUND:
        return -ENODEV;
    default:
        return -EIO;
    }
}

/* ---- bring up ---------------------------------------------------------------------------------- */

static bool s_host_up;

int aic_sd_init(struct sd_card *card, int slot, const sdmmc_slot_config_t *pins)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    esp_err_t e;

    memset(card, 0, sizeof(*card));
    card->slot = slot;
    host.slot = slot;
    if (!s_host_up) {
        e = sdmmc_host_init();
        if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
            return neg(e);
        }
        s_host_up = true;
    }
    e = sdmmc_host_init_slot(slot, pins);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        return neg(e);
    }
    card->c = calloc(1, sizeof(sdmmc_card_t));
    if (!card->c) {
        return -ENOMEM;
    }
    e = sdmmc_card_init(&host, card->c);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "card init: %s", esp_err_to_name(e));
        free(card->c);
        card->c = NULL;
        sdmmc_host_deinit_slot(slot);
        return neg(e);
    }
    if (!card->c->is_sdio) {
        free(card->c);
        card->c = NULL;
        sdmmc_host_deinit_slot(slot);
        return -ENODEV;
    }
    card->num_io = card->c->num_io_functions;
    card->func0.card = card;
    card->func0.num = 0;
    card->card_speed = 0;
    return 0;
}

void aic_sd_deinit(struct sd_card *card)
{
    if (card->c) {
        sdmmc_host_deinit_slot(card->slot);
        free(card->c);
        card->c = NULL;
    }
}

/* ---- functions --------------------------------------------------------------------------------- */

/* manufacturer tuple (0x20) of the CIS the function base registers point to */
static void read_cis(struct sd_card *card, int num, struct sdio_cis *cis)
{
    uint32_t ptr = 0;

    cis->manf_id = cis->manf_code = 0;
    for (int i = 0; i < 3; i++) {
        uint8_t b = 0;

        if (sdmmc_io_read_byte(card->c, 0, SDIO_FBR_BASE(num) + 9 + i, &b) != ESP_OK) {
            return;
        }
        ptr |= (uint32_t)b << (8 * i);
    }
    for (int n = 0; n < 16 && ptr; n++) {
        uint8_t code = 0, link = 0, d[4] = {0};

        if (sdmmc_io_read_byte(card->c, 0, ptr, &code) != ESP_OK || code == 0xff) {
            return;
        }
        if (sdmmc_io_read_byte(card->c, 0, ptr + 1, &link) != ESP_OK || link == 0xff) {
            return;
        }
        if (code == 0x20 && link >= 4) {
            for (int i = 0; i < 4; i++) {
                sdmmc_io_read_byte(card->c, 0, ptr + 2 + i, &d[i]);
            }
            cis->manf_id = d[0] | (d[1] << 8);
            cis->manf_code = d[2] | (d[3] << 8);
            return;
        }
        ptr += 2 + link;
    }
}

int sdio_init_func(struct sd_card *card, struct sdio_func *func, int num)
{
    memset(func, 0, sizeof(*func));
    func->num = num;
    read_cis(card, num, &func->cis);
    if (num == 1) {
        read_cis(card, 0, &card->func0.cis);
    }
    func->card = card;
    func->block_size = 0;
    return sdio_set_block_size(func, 512);
}

int sdio_enable_func(struct sdio_func *func)
{
    uint8_t en = 0, rdy = 0;
    int ret = sdio_read_byte(&func->card->func0, 0x02, &en);

    if (ret) {
        return ret;
    }
    ret = sdio_write_byte(&func->card->func0, 0x02, en | BIT(func->num));
    if (ret) {
        return ret;
    }
    for (int i = 0; i < 100; i++) {
        ret = sdio_read_byte(&func->card->func0, 0x03, &rdy);
        if (ret == 0 && (rdy & BIT(func->num))) {
            return 0;
        }
        vTaskDelay(1);
    }
    return -ETIMEDOUT;
}

int sdio_set_block_size(struct sdio_func *func, unsigned int blksize)
{
    int ret = sdio_write_byte(&func->card->func0, SDIO_FBR_BASE(func->num) + 0x10, blksize & 0xff);

    if (ret == 0) {
        ret = sdio_write_byte(&func->card->func0, SDIO_FBR_BASE(func->num) + 0x11, (blksize >> 8) & 0xff);
    }
    if (ret == 0) {
        func->block_size = blksize;
    }
    return ret;
}

int sdio_read_byte(struct sdio_func *func, uint32_t reg, uint8_t *val)
{
    return neg(sdmmc_io_read_byte(func->card->c, func->num, reg, val));
}

int sdio_write_byte(struct sdio_func *func, uint32_t reg, uint8_t val)
{
    return neg(sdmmc_io_write_byte(func->card->c, func->num, reg, val, NULL));
}

int sdio_read_addr(struct sdio_func *func, uint32_t addr, void *dst, size_t size)
{
    return neg(sdmmc_io_read_bytes(func->card->c, func->num, addr, dst, size));
}

int sdio_write_addr(struct sdio_func *func, uint32_t addr, const void *src, size_t size)
{
    return neg(sdmmc_io_write_bytes(func->card->c, func->num, addr, src, size));
}

int sdio_read_fifo(struct sdio_func *func, uint32_t addr, void *dst, size_t size)
{
    return neg(sdmmc_io_read_bytes(func->card->c, func->num, addr | SDMMC_IO_FIXED_ADDR, dst, size));
}

int sdio_write_fifo(struct sdio_func *func, uint32_t addr, const void *src, size_t size)
{
    return neg(sdmmc_io_write_bytes(func->card->c, func->num, addr | SDMMC_IO_FIXED_ADDR, src, size));
}

int sdio_read_blocks_fifo(struct sdio_func *func, uint32_t addr, void *dst, size_t blocks)
{
    return neg(sdmmc_io_read_blocks(func->card->c, func->num, addr | SDMMC_IO_FIXED_ADDR, dst, blocks * func->block_size));
}

int sdio_write_blocks_fifo(struct sdio_func *func, uint32_t addr, const void *src, size_t blocks)
{
    return neg(sdmmc_io_write_blocks(func->card->c, func->num, addr | SDMMC_IO_FIXED_ADDR, src, blocks * func->block_size));
}

/* ---- card interrupt ---------------------------------------------------------------------------- */

static struct {
    struct sd_card *card;
    void (*cb)(void);
    SemaphoreHandle_t arm;
    TaskHandle_t task;
    volatile bool on;
} s_irq;

static void irq_task(void *arg)
{
    for (;;) {
        xSemaphoreTake(s_irq.arm, portMAX_DELAY);
        if (!s_irq.on || !s_irq.card || !s_irq.card->c) {
            continue;
        }
        if (sdmmc_io_enable_int(s_irq.card->c) != ESP_OK) {
            continue;
        }
        while (s_irq.on) {
            esp_err_t e = sdmmc_io_wait_int(s_irq.card->c, pdMS_TO_TICKS(500));

            if (e == ESP_OK) {
                if (s_irq.cb) {
                    s_irq.cb();
                }
                break;
            }
        }
    }
}

void aic_sd_irq_arm(struct sd_card *card, void (*cb)(void))
{
    if (!s_irq.arm) {
        s_irq.arm = xSemaphoreCreateBinary();
        xTaskCreate(irq_task, "aic_sdirq", 3072, NULL, configMAX_PRIORITIES - 3, &s_irq.task);
    }
    s_irq.card = card;
    s_irq.cb = cb;
    s_irq.on = true;
    xSemaphoreGive(s_irq.arm);
}

void aic_sd_irq_off(void)
{
    s_irq.on = false;
}
