/*
 * SPDX-FileCopyrightText: 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <stdlib.h>
#include "esp_attr.h"
#include "sdkconfig.h"
#include "soc/soc.h"
#include "heap_memory_layout.h"
#include "esp_heap_caps.h"

/* The whole 16 MB PSRAM is one memory: the image sits at its start and the
 * rest behind _heap_start is the heap. DMA needs cache maintenance, which
 * the DMA users do themselves. */
enum {
    SOC_MEMORY_TYPE_RAM = 0,
    SOC_MEMORY_TYPE_NUM,
};

#define F101_MEM_CAPS (MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL | MALLOC_CAP_32BIT | MALLOC_CAP_8BIT | \
                       MALLOC_CAP_EXEC | MALLOC_CAP_DMA)

const soc_memory_type_desc_t soc_memory_types[SOC_MEMORY_TYPE_NUM] = {
    [SOC_MEMORY_TYPE_RAM] = { "RAM", { F101_MEM_CAPS, 0, 0 } },
};

const size_t soc_memory_type_count = sizeof(soc_memory_types) / sizeof(soc_memory_type_desc_t);

#define F101_RAM_START 0x40010000
#define F101_RAM_SIZE  0x00FF0000

const soc_memory_region_t soc_memory_regions[] = {
    { F101_RAM_START, F101_RAM_SIZE, SOC_MEMORY_TYPE_RAM, F101_RAM_START, false },
};

const size_t soc_memory_region_count = sizeof(soc_memory_regions) / sizeof(soc_memory_region_t);

extern int _heap_start;

/* the image: code, data, bss */
SOC_RESERVE_MEMORY_REGION(F101_RAM_START, (intptr_t)&_heap_start, image);
