/*
 * SPDX-FileCopyrightText: 2026 Yuzuki Tsuru
 * SPDX-License-Identifier: Apache-2.0
 *
 * What the video engine core needs from its environment: register access, a mutex, a semaphore, logging
 * and a few macros, on FreeRTOS and the ESP-IDF cache API.
 */
#pragma once

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_cache.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/* configuration of the decoder: JPEG only, the other decoders are not built */
#define CONFIG_VDEC_SUNXI_JPEG          1
#define CONFIG_VDEC_SUNXI_CLOCK_MHZ     300
#define CONFIG_VDEC_SUNXI_STREAM_BUFFER_KB 512
#define CONFIG_VDEC_SUNXI_MAX_MEMORY_KB 12288
#define CONFIG_VDEC_SUNXI_MEM_ALIGN     4096
#define CONFIG_DCACHE_LINE_SIZE         64
#define CONFIG_VDEC_SUNXI_MEM_TRACE     0

#ifndef BIT
#define BIT(n)              (1UL << (n))
#endif
#ifndef GENMASK
#define GENMASK(h, l)       (((~0UL) << (l)) & (~0UL >> (31 - (h))))
#endif
#define FIELD_SHIFT(mask)   ((unsigned)__builtin_ctzl(mask))
#define FIELD_PREP(mask, v) ((((uint32_t)(v)) << FIELD_SHIFT(mask)) & (mask))
#define FIELD_GET(mask, v)  (((uint32_t)(v) & (mask)) >> FIELD_SHIFT(mask))
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a)       (sizeof(a) / sizeof((a)[0]))
#endif
#ifndef MIN
#define MIN(a, b)           ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b)           ((a) > (b) ? (a) : (b))
#endif
#ifndef CLAMP
#define CLAMP(v, lo, hi)    ((v) < (lo) ? (lo) : ((v) > (hi) ? (hi) : (v)))
#endif
#ifndef ROUND_UP
#define ROUND_UP(x, a)      ((((x) + (a) - 1) / (a)) * (a))
#endif
#ifndef ROUND_DOWN
#define ROUND_DOWN(x, a)    (((x) / (a)) * (a))
#endif
#ifndef ARG_UNUSED
#define ARG_UNUSED(x)       (void)(x)
#endif
#define IS_ENABLED(x)       (x)
#ifndef __aligned
#define __aligned(n)        __attribute__((aligned(n)))
#endif

typedef uintptr_t mem_addr_t;
static inline uint32_t sys_read32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void sys_write32(uint32_t v, uintptr_t a) { *(volatile uint32_t *)a = v; }
static inline void sys_write16(uint16_t v, uintptr_t a) { *(volatile uint16_t *)a = v; }
static inline void sys_write8(uint8_t v, uintptr_t a) { *(volatile uint8_t *)a = v; }

/* logging */
#define LOG_ERR(...)        ESP_LOGE("ve", __VA_ARGS__)
#define LOG_WRN(...)        ESP_LOGW("ve", __VA_ARGS__)
#define LOG_INF(...)        ESP_LOGI("ve", __VA_ARGS__)
#define LOG_DBG(...)        do { } while (0)
#define VE_LOG_DECLARE()    struct ve_log_unused_
#define printk(...)         do { } while (0)

/* mutex: a FreeRTOS mutex created on first use */
struct k_mutex {
    SemaphoreHandle_t h;
};
#define K_FOREVER           (-1)
static inline void k_mutex_init(struct k_mutex *m)
{
    m->h = xSemaphoreCreateRecursiveMutex();
}
static inline int k_mutex_lock(struct k_mutex *m, int timeout)
{
    if (!m->h) {
        m->h = xSemaphoreCreateRecursiveMutex();
    }
    xSemaphoreTakeRecursive(m->h, portMAX_DELAY);
    return 0;
}
static inline int k_mutex_unlock(struct k_mutex *m)
{
    xSemaphoreGiveRecursive(m->h);
    return 0;
}
#define Z_MUTEX_INITIALIZER(m) { 0 }

/* cache */
static inline void sys_cache_data_flush_and_invd_range(void *a, size_t n)
{
    esp_cache_msync(a, n, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE | ESP_CACHE_MSYNC_FLAG_TYPE_DATA |
                              ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}
