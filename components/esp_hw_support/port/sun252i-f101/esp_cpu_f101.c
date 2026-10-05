/*
 * SPDX-FileCopyrightText: 2026 Yuzuki Tsuru
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <stdbool.h>
#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_cpu.h"
#include "esp_rom_sys.h"

void esp_cpu_stall(int core_id)
{
    (void)core_id;
}

void esp_cpu_unstall(int core_id)
{
    (void)core_id;
}

void esp_cpu_reset(int core_id)
{
    (void)core_id;
    esp_rom_software_reset_system();
}

void esp_cpu_wait_for_intr(void)
{
    __asm volatile("wfi");
}

void esp_cpu_configure_region_protection(void)
{
}

esp_err_t esp_cpu_set_breakpoint(int bp_num, const void *bp_addr)
{
    (void)bp_num; (void)bp_addr;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esp_cpu_clear_breakpoint(int bp_num)
{
    (void)bp_num;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esp_cpu_set_watchpoint(int wp_num, const void *wp_addr, size_t size, esp_cpu_watchpoint_trigger_t trigger)
{
    (void)wp_num; (void)wp_addr; (void)size; (void)trigger;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esp_cpu_clear_watchpoint(int wp_num)
{
    (void)wp_num;
    return ESP_ERR_NOT_SUPPORTED;
}

bool esp_cpu_compare_and_set(volatile uint32_t *addr, uint32_t compare_value, uint32_t new_value)
{
    uint32_t old;
    uint32_t mstatus;

    __asm volatile("csrrci %0, mstatus, 8" : "=r"(mstatus));
    old = *addr;
    if (old == compare_value) {
        *addr = new_value;
    }
    if (mstatus & 8) {
        __asm volatile("csrsi mstatus, 8");
    }
    return old == compare_value;
}
