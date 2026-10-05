/*
 * SPDX-FileCopyrightText: 2026 Yuzuki Tsuru
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <stdbool.h>
#include "sdkconfig.h"
#include "esp_attr.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_private/system_internal.h"
#include <string.h>
#include "esp_private/panic_internal.h"
#include "esp_private/cache_err_int.h"

void __attribute__((noreturn)) esp_restart_noos(void)
{
    esp_rom_software_reset_system();
    __builtin_unreachable();
}

void __attribute__((noreturn)) esp_restart_noos_dig(void)
{
    esp_restart_noos();
}

void esp_system_reset_modules_on_exit(void)
{
}

esp_reset_reason_t esp_reset_reason(void)
{
    return ESP_RST_POWERON;
}

void esp_reset_reason_set_hint(esp_reset_reason_t hint)
{
    (void)hint;
}

esp_reset_reason_t esp_reset_reason_get_hint(void)
{
    return ESP_RST_UNKNOWN;
}

/* no cache, no flash: nothing can fault on a cache access */
bool esp_cache_err_has_active_err(void)
{
    return false;
}

void esp_cache_err_clear_active_err(void)
{
}

int esp_cache_err_get_cpuid(void)
{
    return -1;
}

void esp_cache_err_get_panic_info(esp_cache_err_info_t *err_info)
{
    memset(err_info, 0, sizeof(*err_info));
}

void esp_cache_err_int_init(void)
{
}

void esp_crosscore_int_init(void)
{
}
