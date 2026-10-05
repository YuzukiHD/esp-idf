/*
 * SPDX-FileCopyrightText: 2026 Yuzuki Tsuru
 * SPDX-License-Identifier: Apache-2.0
 *
 * esp_timer on the 24 MHz CLINT counter. The single mtimecmp serves both the
 * FreeRTOS tick and the esp_timer alarm: the FreeRTOS port programs it to the
 * earlier one and calls the alarm callback registered here.
 */

#include <sys/param.h>
#include "sdkconfig.h"
#include "esp_timer_impl.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_private/esp_clk.h"
#include "freertos/FreeRTOS.h"
#include "soc/sun252i_f101_ll.h"

#define TICKS_PER_US    (F101_MTIME_HZ / 1000000u)

extern portMUX_TYPE s_time_update_lock;
extern uint64_t timestamp_id[2];

/* provided by the FreeRTOS port */
extern void f101_timer_set_alarm_mtime(uint64_t mtime_deadline);
extern void f101_timer_set_alarm_cb(void (*cb)(void *), void *arg);

static int64_t s_offset_us;
static intr_handler_t s_alarm_handler;

static uint64_t raw_us(void)
{
    return f101_mtime_get() / TICKS_PER_US;
}

int64_t IRAM_ATTR esp_timer_impl_get_time(void)
{
    return (int64_t)raw_us() + s_offset_us;
}

int64_t esp_timer_get_time(void) __attribute__((alias("esp_timer_impl_get_time")));

uint64_t IRAM_ATTR esp_timer_impl_get_counter_reg(void)
{
    return f101_mtime_get();
}

void IRAM_ATTR esp_timer_impl_set_alarm_id(uint64_t timestamp, unsigned alarm_id)
{
    assert(alarm_id < sizeof(timestamp_id) / sizeof(timestamp_id[0]));
    portENTER_CRITICAL_SAFE(&s_time_update_lock);
    timestamp_id[alarm_id] = timestamp;
    timestamp = MIN(timestamp_id[0], timestamp_id[1]);
    if (timestamp == UINT64_MAX) {
        f101_timer_set_alarm_mtime(UINT64_MAX);
    } else {
        f101_timer_set_alarm_mtime((uint64_t)((int64_t)timestamp - s_offset_us) * TICKS_PER_US);
    }
    portEXIT_CRITICAL_SAFE(&s_time_update_lock);
}

void esp_timer_impl_set(uint64_t new_us)
{
    portENTER_CRITICAL_SAFE(&s_time_update_lock);
    s_offset_us = (int64_t)new_us - (int64_t)raw_us();
    portEXIT_CRITICAL_SAFE(&s_time_update_lock);
}

void esp_timer_impl_advance(int64_t time_diff_us)
{
    portENTER_CRITICAL_SAFE(&s_time_update_lock);
    s_offset_us += time_diff_us;
    portEXIT_CRITICAL_SAFE(&s_time_update_lock);
}

esp_err_t esp_timer_impl_early_init(void)
{
    return ESP_OK;
}

esp_err_t esp_timer_impl_init(intr_handler_t alarm_handler)
{
    s_alarm_handler = alarm_handler;
    f101_timer_set_alarm_cb(alarm_handler, NULL);
    return ESP_OK;
}

void esp_timer_impl_deinit(void)
{
    f101_timer_set_alarm_mtime(UINT64_MAX);
    f101_timer_set_alarm_cb(NULL, NULL);
    s_alarm_handler = NULL;
}

uint64_t esp_timer_impl_get_alarm_reg(void)
{
    return MIN(timestamp_id[0], timestamp_id[1]);
}

void esp_timer_private_set(uint64_t new_us) __attribute__((alias("esp_timer_impl_set")));
void esp_timer_private_advance(int64_t time_diff_us) __attribute__((alias("esp_timer_impl_advance")));
