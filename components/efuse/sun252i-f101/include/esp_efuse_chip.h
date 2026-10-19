/*
 * SPDX-FileCopyrightText: 2026 Yuzuki Tsuru
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EFUSE_BLK0      = 0,    /**< the 512 fuse bits behind the SID controller */
    EFUSE_BLK_MAX   = 1,
} esp_efuse_block_t;

typedef enum {
    EFUSE_CODING_SCHEME_NONE = 0,
} esp_efuse_coding_scheme_t;

typedef enum {
    ESP_EFUSE_KEY_PURPOSE_USER = 0,
    ESP_EFUSE_KEY_PURPOSE_MAX,
} esp_efuse_purpose_t;

#ifdef __cplusplus
}
#endif
