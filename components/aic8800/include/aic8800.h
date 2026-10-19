/*
 * SPDX-FileCopyrightText: 2026 Yuzuki Tsuru
 * SPDX-License-Identifier: Apache-2.0
 *
 * AIC8800 WiFi chip as the chip driver of the esp_wifi API (station mode).
 */
#pragma once

#include "esp_err.h"
#include "driver/sdmmc_host.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int sdmmc_slot;         /**< SD host slot the chip is wired to */
    sdmmc_slot_config_t sdmmc_pins; /**< pads of that slot */
    int reg_on_gpio;        /**< power enable of the chip */
    int wakeup_gpio;        /**< wake-up input, -1 if not wired */
} aic8800_config_t;

/** Register the chip with esp_wifi; esp_wifi_start() then powers it up and loads the firmware. */
esp_err_t aic8800_wifi_register(const aic8800_config_t *config);

#ifdef __cplusplus
}
#endif
