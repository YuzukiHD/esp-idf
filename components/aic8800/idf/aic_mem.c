/*
 * SPDX-FileCopyrightText: 2026 Yuzuki Tsuru
 * SPDX-License-Identifier: Apache-2.0
 *
 * The memory of the WiFi driver: one block of CONFIG_AIC8800_MEM_POOL_KB, taken from the heap when the driver
 * first asks for memory and given back as a whole when the chip is stopped (aic_mem_release()), managed as a
 * heap of its own in between. A request the pool cannot serve fails. A pointer that is not from the pool
 * (a string from strdup) goes back to the C library.
 */
#include <stdint.h>
#include <stdio.h>
#include "sdkconfig.h"
#include "esp_heap_caps.h"
#include "multi_heap.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#undef malloc
#undef calloc
#undef realloc
#undef aligned_alloc

extern void __real_free(void *ptr);
extern void *realloc(void *ptr, size_t size);

#define POOL_BYTES  (CONFIG_AIC8800_MEM_POOL_KB * 1024)
#define DEFAULT_ALIGN 8

static uint8_t *s_pool;
static multi_heap_handle_t s_heap;
static SemaphoreHandle_t s_lock;
static bool s_warned;

static bool in_pool(const void *p)
{
    return s_pool && (const uint8_t *)p >= s_pool && (const uint8_t *)p < s_pool + POOL_BYTES;
}

static bool ensure_ready(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateRecursiveMutex();
    }
    if (!s_heap) {
        s_pool = heap_caps_aligned_alloc(64, POOL_BYTES, MALLOC_CAP_DEFAULT);
        if (!s_pool) {
            return false;
        }
        s_heap = multi_heap_register(s_pool, POOL_BYTES);
        if (!s_heap) {
            heap_caps_free(s_pool);
            s_pool = NULL;
            return false;
        }
        s_warned = false;
    }
    return true;
}

static void *pool_alloc(size_t align, size_t size)
{
    void *p = NULL;

    xSemaphoreTakeRecursive(s_lock ? s_lock : (ensure_ready(), s_lock), portMAX_DELAY);
    if (ensure_ready()) {
        p = multi_heap_aligned_alloc(s_heap, size ? size : 1, align);
    }
    xSemaphoreGiveRecursive(s_lock);
    if (!p && !s_warned) {
        s_warned = true;
        printf("aic8800: memory pool of %u KiB is full (%u bytes asked)\n", (unsigned)(POOL_BYTES / 1024), (unsigned)size);
    }
    return p;
}

void *aic_mem_malloc(size_t size)
{
    return pool_alloc(DEFAULT_ALIGN, size);
}

void *aic_mem_aligned_alloc(size_t align, size_t size)
{
    return pool_alloc(align > DEFAULT_ALIGN ? align : DEFAULT_ALIGN, size);
}

void *aic_mem_calloc(size_t n, size_t size)
{
    if (size && n > SIZE_MAX / size) {
        return NULL;
    }
    void *p = pool_alloc(DEFAULT_ALIGN, n * size);

    if (p) {
        memset(p, 0, n * size);
    }
    return p;
}

void *aic_mem_realloc(void *ptr, size_t size)
{
    if (!ptr) {
        return pool_alloc(DEFAULT_ALIGN, size);
    }
    if (!in_pool(ptr)) {
        return realloc(ptr, size);
    }
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    void *p = multi_heap_realloc(s_heap, ptr, size);

    xSemaphoreGiveRecursive(s_lock);
    return p;
}

/* every free() of the image (--wrap=free): a pointer of the pool goes back to the pool */
void __wrap_free(void *ptr)
{
    if (!ptr) {
        return;
    }
    if (!in_pool(ptr)) {
        __real_free(ptr);
        return;
    }
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    multi_heap_free(s_heap, ptr);
    xSemaphoreGiveRecursive(s_lock);
}

void aic_mem_stats(size_t *used, size_t *peak)
{
    multi_heap_info_t info = {0};

    if (s_heap) {
        multi_heap_get_info(s_heap, &info);
    }
    *used = info.total_allocated_bytes;
    *peak = POOL_BYTES - multi_heap_minimum_free_size(s_heap ? s_heap : NULL);
}

/* Gives the whole pool back. Everything the driver allocated is gone with it: for the end of a driver that
 * has no task and no timer left. */
void aic_mem_release(void)
{
    if (!s_lock) {
        return;
    }
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    if (s_heap) {
        s_heap = NULL;
        heap_caps_free(s_pool);
        s_pool = NULL;
    }
    xSemaphoreGiveRecursive(s_lock);
}
