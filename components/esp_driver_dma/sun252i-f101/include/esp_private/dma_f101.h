/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

/*
 * Linked-list DMA of the sun252i-f101 target: 12 channels, each channel moves data between
 * memory and memory, memory and a peripheral FIFO (M2P) or a peripheral FIFO and memory (P2M).
 * A transfer is a chain of blocks; the chain can run in a cycle. The peripheral side address
 * of a block is a FIFO, it is not incremented.
 *
 * This is the layer under esp_async_memcpy and the I2S driver, not a public ESP-IDF API.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct f101_dma_channel *f101_dma_handle_t;

typedef enum {
    F101_DMA_M2M,
    F101_DMA_M2P,           /*!< memory to the FIFO at the destination address */
    F101_DMA_P2M,           /*!< FIFO at the source address to memory */
} f101_dma_dir_t;

typedef enum {
    F101_DMA_EVENT_BLOCK,   /*!< one block of the chain is done, the transfer goes on */
    F101_DMA_EVENT_DONE,    /*!< the chain is done (not raised for a cyclic chain) */
    F101_DMA_EVENT_ERROR,
} f101_dma_event_t;

/** Called in interrupt context; returns true when a higher priority task was woken */
typedef bool (*f101_dma_cb_t)(f101_dma_handle_t chan, f101_dma_event_t event, void *arg);

typedef struct {
    f101_dma_dir_t dir;
    uint32_t slot;          /*!< request line of the peripheral (M2P, P2M) */
    uint8_t width;          /*!< bytes per beat: 1, 2 or 4 */
    uint8_t burst;          /*!< beats per burst: 1, 4, 8 or 16 */
    bool cyclic;            /*!< the last block continues with the first one */
    bool block_events;      /*!< raise F101_DMA_EVENT_BLOCK at the end of every block */
    f101_dma_cb_t cb;
    void *cb_arg;
} f101_dma_config_t;

typedef struct {
    uint32_t src;           /*!< physical addresses */
    uint32_t dst;
    uint32_t len;           /*!< bytes, up to 16 MiB - 1 */
} f101_dma_block_t;

esp_err_t f101_dma_new_channel(f101_dma_handle_t *ret_chan);
esp_err_t f101_dma_del_channel(f101_dma_handle_t chan);

/** Set up the chain and start it. The source buffers must have been written back from the cache. */
esp_err_t f101_dma_start(f101_dma_handle_t chan, const f101_dma_config_t *config, const f101_dma_block_t *blocks, size_t num_blocks);

/** Stop the channel, the chain is released */
esp_err_t f101_dma_stop(f101_dma_handle_t chan);

bool f101_dma_is_busy(f101_dma_handle_t chan);

/** Offset in the chain of the memory side address the channel is working on, in bytes */
esp_err_t f101_dma_get_position(f101_dma_handle_t chan, uint32_t *out_offset);

int f101_dma_get_channel_id(f101_dma_handle_t chan);

#ifdef __cplusplus
}
#endif
