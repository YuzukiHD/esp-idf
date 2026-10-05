/*
 * SPDX-FileCopyrightText: 2026 Yuzuki Tsuru
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stddef.h>
#include <stdint.h>
#include "esp_private/heap_align_hw.h"

/* the DMA engines of this SoC work on cache lines of 64 bytes */
void esp_heap_adjust_alignment_to_hw(size_t *p_alignment, size_t *p_size, uint32_t *p_caps)
{
    (void)p_size;
    (void)p_caps;
    (void)p_alignment;
}
