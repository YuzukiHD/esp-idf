/*
 * SPDX-FileCopyrightText: 2026 Yuzuki Tsuru
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <time.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Read the real time clock (calendar time of the clock, no time zone applied).
 *
 * @return ESP_ERR_INVALID_STATE when the clock has never been set
 */
esp_err_t f101_rtc_get_time(struct tm *tm);

/**
 * @brief Set the real time clock; years 1970..2097.
 */
esp_err_t f101_rtc_set_time(const struct tm *tm);

/**
 * @brief Set the real time clock from the system time and the system time from the clock.
 *
 * At start the system time is taken from the clock when it has been set. After
 * settimeofday() call f101_rtc_save_system_time() to keep the time over a power cycle.
 */
esp_err_t f101_rtc_save_system_time(void);

#ifdef __cplusplus
}
#endif
