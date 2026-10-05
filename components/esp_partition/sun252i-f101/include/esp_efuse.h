/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>

/* no eFuse block with flash encryption on this target */
static inline bool esp_efuse_is_flash_encryption_enabled(void)
{
    return false;
}
