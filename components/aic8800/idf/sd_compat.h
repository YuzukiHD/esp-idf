/*
 * SPDX-FileCopyrightText: 2026 Yuzuki Tsuru
 * SPDX-License-Identifier: Apache-2.0
 *
 * The SDIO function interface the transport of the chip (sdio_port.c) is written against, on the sdmmc
 * driver: a card with up to three I/O functions, byte and FIFO access, a one-shot card interrupt.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "sdmmc_cmd.h"
#include "driver/sdmmc_types.h"
#include "driver/sdmmc_host.h"

#ifndef BIT
#define BIT(n) (1UL << (n))
#endif

#define SDIO_CCCR_INT_EN        0x04
#define SDIO_FBR_BASE(f)        ((f) * 0x100)
#define SD_TIMING_DDR50         0xdd50

struct sd_card;

struct sdio_cis {
    uint16_t manf_id;           /**< manufacturer code of the tuple (the vendor) */
    uint16_t manf_code;         /**< manufacturer information of the tuple (the card id) */
    uint16_t max_blk_size;
};

struct sdio_func {
    struct sd_card *card;
    uint8_t num;
    uint32_t block_size;
    struct sdio_cis cis;
};

struct sd_card {
    sdmmc_card_t *c;
    struct sdio_func func0;
    int num_io;
    int card_speed;
    int slot;
};

/** Bring the card up on the host slot (power has to be on); 0 or a negative errno. */
int aic_sd_init(struct sd_card *card, int slot, const sdmmc_slot_config_t *pins);
void aic_sd_deinit(struct sd_card *card);

int sdio_init_func(struct sd_card *card, struct sdio_func *func, int num);
int sdio_enable_func(struct sdio_func *func);
int sdio_set_block_size(struct sdio_func *func, unsigned int blksize);
int sdio_read_byte(struct sdio_func *func, uint32_t reg, uint8_t *val);
int sdio_write_byte(struct sdio_func *func, uint32_t reg, uint8_t val);
int sdio_read_addr(struct sdio_func *func, uint32_t addr, void *dst, size_t size);
int sdio_write_addr(struct sdio_func *func, uint32_t addr, const void *src, size_t size);
int sdio_read_fifo(struct sdio_func *func, uint32_t addr, void *dst, size_t size);
int sdio_write_fifo(struct sdio_func *func, uint32_t addr, const void *src, size_t size);
int sdio_read_blocks_fifo(struct sdio_func *func, uint32_t addr, void *dst, size_t blocks);
int sdio_write_blocks_fifo(struct sdio_func *func, uint32_t addr, const void *src, size_t blocks);

/** Card interrupt: @p cb runs in a task when the card asserts DAT1; it is one-shot, arm it again afterwards. */
void aic_sd_irq_arm(struct sd_card *card, void (*cb)(void));
void aic_sd_irq_off(void);
