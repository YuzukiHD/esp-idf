/*
 * SPDX-FileCopyrightText: 2026 Yuzuki Tsuru
 * SPDX-License-Identifier: Apache-2.0
 *
 * The few helpers the accelerator core uses.
 */
#pragma once

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include "esp_cache.h"

#ifndef BIT
#define BIT(n)              (1UL << (n))
#endif
#define G2D_GENMASK(h, l)   (((~0UL) << (l)) & (~0UL >> (31 - (h))))
#ifndef GENMASK
#define GENMASK(h, l)       G2D_GENMASK(h, l)
#endif
#define FIELD_SHIFT(mask)   ((unsigned)__builtin_ctzl(mask))
#define FIELD_PREP(mask, v) ((((uint32_t)(v)) << FIELD_SHIFT(mask)) & (mask))
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a)       (sizeof(a) / sizeof((a)[0]))
#endif
#ifndef MIN
#define MIN(a, b)           ((a) < (b) ? (a) : (b))
#endif
#define ROUND_UP(x, a)      ((((x) + (a) - 1) / (a)) * (a))
#define LOG_DBG(...)        do { } while (0)
#define sys_cache_data_flush_range(addr, len) \
    esp_cache_msync((void *)(addr), (len), ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA | ESP_CACHE_MSYNC_FLAG_UNALIGNED)
#define k_malloc            malloc
#define k_free              free
#define BUILD_ASSERT(c, ...) _Static_assert(c, "" __VA_ARGS__)
#define __ASSERT_NO_MSG(c)  assert(c)
#define __ASSERT(c, ...)    assert(c)
#ifndef CLAMP
#define CLAMP(v, lo, hi)    ((v) < (lo) ? (lo) : ((v) > (hi) ? (hi) : (v)))
#define MAX(a, b)           ((a) > (b) ? (a) : (b))
#define DIV_ROUND_UP(n, d)  (((n) + (d) - 1) / (d))
#endif
