/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
/* no flash cache: strings and code are always reachable */
static inline bool spi_flash_cache_enabled(void) { return true; }
static inline bool esp_task_stack_is_sane_cache_disabled(void) { return true; }
static inline void spi_flash_enable_cache(unsigned int cpuid) { (void)cpuid; }
