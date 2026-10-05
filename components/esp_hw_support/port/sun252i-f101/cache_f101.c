/*
 * SPDX-FileCopyrightText: 2026 Yuzuki Tsuru
 * SPDX-License-Identifier: Apache-2.0
 *
 * Data cache maintenance with the T-Head cache instructions, written as raw
 * encodings (th.dcache.cpa / ipa / cipa with the address in a0) so that the
 * compiler needs no vendor extension.
 */

#include <stdint.h>
#include "esp_cache.h"

#define LINE 64u

static inline void sync_all(void)
{
    __asm volatile("fence\n fence.i" ::: "memory");
}

esp_err_t esp_cache_msync(void *addr, size_t size, int flags)
{
    if (!addr || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!(flags & (ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_DIR_M2C))) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!(flags & ESP_CACHE_MSYNC_FLAG_UNALIGNED) && ((uintptr_t)addr % LINE || size % LINE)) {
        return ESP_ERR_INVALID_ARG;
    }

    uintptr_t a = (uintptr_t)addr & ~(uintptr_t)(LINE - 1);
    uintptr_t e = ((uintptr_t)addr + size + LINE - 1) & ~(uintptr_t)(LINE - 1);
    bool clean = flags & ESP_CACHE_MSYNC_FLAG_DIR_C2M;
    bool inv = (flags & ESP_CACHE_MSYNC_FLAG_DIR_M2C) || (flags & ESP_CACHE_MSYNC_FLAG_INVALIDATE);

    sync_all();
    for (; a < e; a += LINE) {
        register uintptr_t r asm("a0") = a;
        if (clean && inv) {
            __asm volatile(".word 0x02b5000b" :: "r"(r) : "memory");   /* th.dcache.cipa a0 */
        } else if (clean) {
            __asm volatile(".word 0x0295000b" :: "r"(r) : "memory");   /* th.dcache.cpa a0 */
        } else {
            __asm volatile(".word 0x02a5000b" :: "r"(r) : "memory");   /* th.dcache.ipa a0 */
        }
    }
    sync_all();
    return ESP_OK;
}

esp_err_t esp_cache_get_alignment(uint32_t heap_caps, size_t *out_alignment)
{
    if (!out_alignment) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_alignment = LINE;
    return ESP_OK;
}
