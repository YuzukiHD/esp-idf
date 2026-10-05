/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "sd_protocol_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** SD/MMC host of the SMHC0 slot, to be used with sdmmc_card_init() */
sdmmc_host_t f101_sdmmc_host(void);

#ifdef __cplusplus
}
#endif
