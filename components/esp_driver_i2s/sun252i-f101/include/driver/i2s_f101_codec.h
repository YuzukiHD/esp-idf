/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

/*
 * Analog side of the on-chip audio codec (I2S_NUM_0 of the sun252i-f101 target): output volume and
 * mute, the input source and its gain. The headphone driver and the speaker amplifier are powered
 * with the TX channel: they are switched on in i2s_channel_enable() and off in i2s_channel_disable().
 */

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    I2S_F101_INPUT_NONE,
    I2S_F101_INPUT_MIC,         /*!< microphone through the PGA */
    I2S_F101_INPUT_FMIN,
    I2S_F101_INPUT_LINEIN,
} i2s_f101_input_t;

/** Digital DAC volume of both channels, 0 (silent) .. 100 */
esp_err_t i2s_f101_codec_set_volume(int percent);

/** Mute the output without stopping the stream */
esp_err_t i2s_f101_codec_set_mute(bool mute);

/** Headphone driver gain, 0 (lowest) .. 7 */
esp_err_t i2s_f101_codec_set_hp_gain(int gain);

/** Digital ADC volume 0 .. 100 */
esp_err_t i2s_f101_codec_set_input_volume(int percent);

/** Microphone PGA gain 0 .. 15 */
esp_err_t i2s_f101_codec_set_mic_gain(int gain);

esp_err_t i2s_f101_codec_select_input(i2s_f101_input_t input);

#ifdef __cplusplus
}
#endif
