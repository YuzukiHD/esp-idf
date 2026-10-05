/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "hal/spi_flash_types.h"
#include "hal/spi_types.h"

/*
 * There is no memory-mapped SPI flash controller here: the flash host is built on driver/spi_master.h
 * (spi_flash_host_f101.c). The esp_flash layer expects the host instance to be a "memspi" context whose
 * `spi` member is set once the bus device is usable, and a few configuration types.
 */
typedef struct {
    spi_flash_host_inst_t inst;     ///< host driver table, must be first
    void *spi;                      ///< the bus device of this chip, non NULL when initialised
    uint32_t slicer_flags;
} spi_flash_hal_context_t;

typedef struct {
    uint32_t extra_dummy;
    uint32_t fdummy_rin;
    uint32_t cs_hold;
    uint8_t cs_setup;
} spi_flash_hal_timing_config_t;

typedef struct {
    spi_host_device_t host_id;
    int cs_num;
    int freq_mhz;
    esp_flash_io_mode_t default_io_mode;
} spi_flash_hal_config_t;
