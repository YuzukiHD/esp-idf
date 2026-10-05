/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stddef.h>
#include "esp_cache.h"

/* data cache maintenance of arbitrary (not line aligned) buffers, on top of esp_cache_msync() */
static inline void f101_dcache_clean(const void *ptr, size_t size)
{
    esp_cache_msync((void *)ptr, size, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

static inline void f101_dcache_invalidate(void *ptr, size_t size)
{
    esp_cache_msync(ptr, size, ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_DATA | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

static inline void f101_dcache_clean_invalidate(void *ptr, size_t size)
{
    esp_cache_msync(ptr, size, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE | ESP_CACHE_MSYNC_FLAG_TYPE_DATA | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}
