/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stddef.h>

static inline bool bootloader_util_regions_overlap(const size_t start1, const size_t end1, const size_t start2, const size_t end2)
{
    return start1 < end2 && start2 < end1;
}
