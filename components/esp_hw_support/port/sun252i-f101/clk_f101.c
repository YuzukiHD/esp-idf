/*
 * SPDX-FileCopyrightText: 2026 Yuzuki Tsuru
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include "sdkconfig.h"
#include "esp_private/esp_clk.h"
#include "soc/sun252i_f101_ll.h"

int esp_clk_cpu_freq(void)
{
    return CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1000000;
}

int esp_clk_apb_freq(void)
{
    return 100000000;
}

int esp_clk_xtal_freq(void)
{
    return 24000000;
}

uint32_t esp_clk_slowclk_cal_get(void)
{
    return 0;
}

void esp_clk_slowclk_cal_set(uint32_t value)
{
    (void)value;
}

/* microseconds since power up */
uint64_t esp_clk_rtc_time(void)
{
    return f101_mtime_get() / (F101_MTIME_HZ / 1000000u);
}

void esp_clk_private_lock(void)
{
}

void esp_clk_private_unlock(void)
{
}

#include "esp_rtc_time.h"

uint64_t esp_rtc_get_time_us(void)
{
    return esp_clk_rtc_time();
}
