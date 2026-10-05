/*
 * SPDX-FileCopyrightText: 2026 Yuzuki Tsuru
 * SPDX-License-Identifier: Apache-2.0
 *
 * esp_adc/adc_oneshot.h on the 12 bit GPADC (4 usable channels, 1.8 V full scale).
 */

#include <stdlib.h>
#include "sdkconfig.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "soc/sun252i_f101_ll.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define GPADC_BASE      0x02009000u
#define GPADC_BGR       (F101_CCU_BASE + 0x9ecu)
#define GP_SR           0x00u
#define GP_CTRL         0x04u
#define GP_CS_EN        0x08u
#define GP_DATA_INTC    0x28u
#define GP_DATA_INTS    0x38u
#define GP_CH_DATA(n)   (0x80u + 4u * (n))
#define NUM_CHANNELS    4          /* channels 4..11 of the block are not bonded out: conversions never finish */

static const char *TAG = "adc";

struct adc_oneshot_unit_ctx_t {
    SemaphoreHandle_t lock;
    uint32_t configured;
};

static struct adc_oneshot_unit_ctx_t *s_unit;

static inline uint32_t rd(uint32_t off)
{
    return F101_REG32(GPADC_BASE + off);
}

static inline void wr(uint32_t off, uint32_t v)
{
    F101_REG32(GPADC_BASE + off) = v;
}

esp_err_t adc_oneshot_new_unit(const adc_oneshot_unit_init_cfg_t *cfg, adc_oneshot_unit_handle_t *ret)
{
    ESP_RETURN_ON_FALSE(cfg && ret, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(cfg->unit_id == ADC_UNIT_1, ESP_ERR_INVALID_ARG, TAG, "only unit 1");
    ESP_RETURN_ON_FALSE(!s_unit, ESP_ERR_NOT_FOUND, TAG, "unit already in use");
    struct adc_oneshot_unit_ctx_t *u = calloc(1, sizeof(*u));
    ESP_RETURN_ON_FALSE(u, ESP_ERR_NO_MEM, TAG, "no memory");
    u->lock = xSemaphoreCreateMutex();

    F101_REG32(GPADC_BGR) &= ~(1u << 16);
    F101_REG32(GPADC_BGR) |= 1u;
    F101_REG32(GPADC_BGR) |= (1u << 16);

    /* 1 kHz sample rate, continuous mode with calibration and the VCM buffer */
    wr(GP_SR, (rd(GP_SR) & 0xffffu) | ((24000000u / 1000u - 1u) << 16));
    wr(GP_CTRL, (rd(GP_CTRL) & ~(3u << 18)) | (2u << 18) | (1u << 17) | (1u << 16) | 1u);
    wr(GP_CS_EN, 0);
    wr(GP_DATA_INTC, 0);
    wr(GP_DATA_INTS, 0xfff);
    s_unit = u;
    *ret = u;
    return ESP_OK;
}

esp_err_t adc_oneshot_config_channel(adc_oneshot_unit_handle_t h, adc_channel_t channel, const adc_oneshot_chan_cfg_t *cfg)
{
    ESP_RETURN_ON_FALSE(h && cfg, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(channel >= 0 && channel < NUM_CHANNELS, ESP_ERR_INVALID_ARG, TAG, "invalid channel");
    ESP_RETURN_ON_FALSE(cfg->bitwidth == ADC_BITWIDTH_12 || cfg->bitwidth == ADC_BITWIDTH_DEFAULT,
                        ESP_ERR_NOT_SUPPORTED, TAG, "12 bit only");
    h->configured |= 1u << channel;
    return ESP_OK;
}

esp_err_t adc_oneshot_read(adc_oneshot_unit_handle_t h, adc_channel_t chan, int *out_raw)
{
    ESP_RETURN_ON_FALSE(h && out_raw, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(chan >= 0 && chan < NUM_CHANNELS && (h->configured & (1u << chan)), ESP_ERR_INVALID_STATE, TAG,
                        "channel not configured");
    esp_err_t e = ESP_ERR_TIMEOUT;
    uint32_t bit = 1u << chan;

    xSemaphoreTake(h->lock, portMAX_DELAY);
    wr(GP_CS_EN, bit);
    wr(GP_DATA_INTS, bit);
    f101_delay_us(1500);                    /* the input needs about 1.5 ms to settle after switching */
    for (int i = 0; i < 10000; i++) {
        if (rd(GP_DATA_INTS) & bit) {
            *out_raw = (int)(rd(GP_CH_DATA(chan)) & 0xfff);
            wr(GP_DATA_INTS, bit);
            e = ESP_OK;
            break;
        }
        f101_delay_us(1);
    }
    wr(GP_CS_EN, 0);
    xSemaphoreGive(h->lock);
    return e;
}

esp_err_t adc_oneshot_del_unit(adc_oneshot_unit_handle_t h)
{
    ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    wr(GP_CTRL, rd(GP_CTRL) & ~(1u << 16));
    F101_REG32(GPADC_BGR) &= ~((1u << 16) | 1u);
    vSemaphoreDelete(h->lock);
    s_unit = NULL;
    free(h);
    return ESP_OK;
}

esp_err_t adc_oneshot_io_to_channel(int io_num, adc_unit_t *const unit_id, adc_channel_t *const channel)
{
    (void)io_num; (void)unit_id; (void)channel;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t adc_oneshot_channel_to_io(adc_unit_t unit_id, adc_channel_t channel, int *const io_num)
{
    (void)unit_id; (void)channel; (void)io_num;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t adc_oneshot_get_calibrated_result(adc_oneshot_unit_handle_t h, adc_cali_handle_t cali_handle, adc_channel_t chan,
                                            int *cali_result)
{
    int raw;
    (void)cali_handle;
    ESP_RETURN_ON_ERROR(adc_oneshot_read(h, chan, &raw), TAG, "read failed");
    *cali_result = raw * 1800 / 4095;       /* millivolt, 1.8 V full scale */
    return ESP_OK;
}
