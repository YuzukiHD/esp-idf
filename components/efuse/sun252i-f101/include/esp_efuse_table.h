/*
 * SPDX-FileCopyrightText: 2026 Yuzuki Tsuru
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_efuse.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const esp_efuse_desc_t *ESP_EFUSE_CHIP_ID[];     /*!< the 128 bit chip id (fuse words 0..3) */

#ifdef __cplusplus
}
#endif
