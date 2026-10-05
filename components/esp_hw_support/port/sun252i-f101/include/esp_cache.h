/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stddef.h>
#include "esp_err.h"
#include "esp_bit_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Flags of esp_cache_msync(), the same as the ones of the ESP-IDF API */
#define ESP_CACHE_MSYNC_FLAG_INVALIDATE    BIT(0)   /*!< also invalidate the lines after writing them back */
#define ESP_CACHE_MSYNC_FLAG_UNALIGNED     BIT(1)   /*!< allow addresses and sizes that are not cache line aligned */
#define ESP_CACHE_MSYNC_FLAG_DIR_C2M       BIT(2)   /*!< write the data cache back to memory */
#define ESP_CACHE_MSYNC_FLAG_DIR_M2C       BIT(3)   /*!< invalidate the cache lines so that the next read comes from memory */
#define ESP_CACHE_MSYNC_FLAG_TYPE_DATA     BIT(4)
#define ESP_CACHE_MSYNC_FLAG_TYPE_INST     BIT(5)

/**
 * @brief Data cache maintenance of a buffer (64 byte cache lines of the C907)
 *
 * Without ESP_CACHE_MSYNC_FLAG_UNALIGNED the address and the size must be multiples of the line size.
 */
esp_err_t esp_cache_msync(void *addr, size_t size, int flags);

/** @brief Cache line size, the alignment DMA buffers need */
esp_err_t esp_cache_get_alignment(uint32_t heap_caps, size_t *out_alignment);

#ifdef __cplusplus
}
#endif
