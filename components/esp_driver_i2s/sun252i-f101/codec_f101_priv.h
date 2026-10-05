/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* register level access of the codec and its clocks, used by i2s_f101.c */

typedef enum {
    CODEC_DIR_TX,       /*!< DAC */
    CODEC_DIR_RX,       /*!< ADC */
} codec_dir_t;

/** Bring up the bus clock and reset of the codec, set the fixed digital defaults and the analog front end */
esp_err_t codec_init(void);

/** Clocks and FIFO format of a direction. data_bytes is 2 or 4, channels 1 or 2 (RX: 1). */
esp_err_t codec_configure(codec_dir_t dir, uint32_t rate_hz, uint8_t data_bytes, uint8_t channels);
void codec_release(codec_dir_t dir);

/** FIFO addresses for the DMA */
uint32_t codec_fifo_addr(codec_dir_t dir);
#define CODEC_DMA_SLOT      7

/** Start and stop the digital stream, power the analog output for TX */
void codec_start(codec_dir_t dir);
void codec_stop(codec_dir_t dir);
