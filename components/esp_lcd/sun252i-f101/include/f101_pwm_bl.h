/*
 * SPDX-FileCopyrightText: 2026 Yuzuki Tsuru
 * SPDX-License-Identifier: Apache-2.0
 *
 * The backlight PWM block: digital dimming into an analog current sink on up to four output pads.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Route the outputs; gpios[n] is the pad of output n (-1: not used). */
esp_err_t f101_pwm_bl_init(const int gpios[4]);

/** Brightness 0..255. */
esp_err_t f101_pwm_bl_set_level(int level);

#ifdef __cplusplus
}
#endif
