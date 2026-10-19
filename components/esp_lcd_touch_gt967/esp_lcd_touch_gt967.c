/*
 * SPDX-FileCopyrightText: 2026 Yuzuki Tsuru
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_lcd_touch_gt967.h"

static const char *TAG = "gt967";

#define REG_PRODUCT_ID  0x8140
#define REG_STATUS      0x814E
#define REG_CONFIG      0x8047

/* the first three ASCII bytes of the product id, "967" */
#define PRODUCT_ID_967  0x00373639U

/* configuration block: 0x8047..0x8128, checksum at 0x8129 and the "updated" flag at 0x812A */
#define CONFIG_SIZE             228
#define CONFIG_X_MAX            1       /* 16 bit little endian, offsets from REG_CONFIG */
#define CONFIG_Y_MAX            3
#define CONFIG_TOUCH_NUMBER     5       /* low 4 bits */
#define CONFIG_MODULE_SWITCH1   6
#define MODULE_SWITCH1_INT_MASK 0x03    /* 0: rising edge */

#define STATUS_READY            0x80
#define STATUS_POINTS_MASK      0x0F
#define POINT_SIZE              8
#define MAX_POINTS              5
#define JITTER_PX               2

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint8_t checksum(const uint8_t *config)
{
    uint8_t sum = 0;

    for (int i = 0; i < CONFIG_SIZE - 2; i++) {
        sum += config[i];
    }
    return (uint8_t)(~sum + 1);
}

/* Reset with the INT line low (address 0x5d), then INT low for a while before it is released. */
static esp_err_t chip_reset(esp_lcd_touch_handle_t tp)
{
    const gpio_num_t rst = tp->config.rst_gpio_num, in = tp->config.int_gpio_num;

    if (rst == GPIO_NUM_NC || in == GPIO_NUM_NC) {
        return ESP_OK;
    }
    const int rst_active = tp->config.levels.reset;

    ESP_RETURN_ON_ERROR(gpio_set_direction(rst, GPIO_MODE_OUTPUT), TAG, "rst");
    gpio_set_level(rst, rst_active);
    vTaskDelay(pdMS_TO_TICKS(12));
    ESP_RETURN_ON_ERROR(gpio_set_direction(in, GPIO_MODE_OUTPUT), TAG, "int");
    gpio_set_level(in, 0);
    vTaskDelay(pdMS_TO_TICKS(2));
    gpio_set_level(rst, !rst_active);
    vTaskDelay(pdMS_TO_TICKS(10));
    vTaskDelay(pdMS_TO_TICKS(50));
    return gpio_set_direction(in, GPIO_MODE_INPUT);
}

static esp_err_t chip_configure(esp_lcd_touch_handle_t tp)
{
    uint8_t config[CONFIG_SIZE], want[CONFIG_SIZE];

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_rx_param(tp->io, REG_CONFIG, config, CONFIG_SIZE), TAG, "read configuration");
    ESP_RETURN_ON_FALSE(config[0] != 0 && checksum(config) == config[CONFIG_SIZE - 2], ESP_ERR_INVALID_STATE, TAG,
                        "the configuration of the controller is not valid");
    memcpy(want, config, sizeof(want));
    if (tp->config.x_max && tp->config.y_max) {
        want[CONFIG_X_MAX] = tp->config.x_max & 0xff;
        want[CONFIG_X_MAX + 1] = tp->config.x_max >> 8;
        want[CONFIG_Y_MAX] = tp->config.y_max & 0xff;
        want[CONFIG_Y_MAX + 1] = tp->config.y_max >> 8;
    }
    want[CONFIG_TOUCH_NUMBER] = (want[CONFIG_TOUCH_NUMBER] & 0xf0) | (CONFIG_ESP_LCD_TOUCH_MAX_POINTS > MAX_POINTS ? MAX_POINTS : CONFIG_ESP_LCD_TOUCH_MAX_POINTS);
    want[CONFIG_MODULE_SWITCH1] &= ~MODULE_SWITCH1_INT_MASK;
    if (memcmp(config, want, CONFIG_SIZE - 2) == 0) {
        return ESP_OK;
    }
    want[CONFIG_SIZE - 2] = checksum(want);
    want[CONFIG_SIZE - 1] = 1;      /* configuration updated */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(tp->io, REG_CONFIG, want, CONFIG_SIZE), TAG, "write configuration");
    vTaskDelay(pdMS_TO_TICKS(60));
    return ESP_OK;
}

static esp_err_t read_data(esp_lcd_touch_handle_t tp)
{
    uint8_t buf[1 + MAX_POINTS * POINT_SIZE], clear = 0;
    uint8_t n;
    esp_lcd_touch_point_data_t np[MAX_POINTS];

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_rx_param(tp->io, REG_STATUS, buf, 1), TAG, "status");
    if (!(buf[0] & STATUS_READY)) {
        return ESP_OK;                  /* no new frame: the previous points stay */
    }
    n = buf[0] & STATUS_POINTS_MASK;
    if (n > CONFIG_ESP_LCD_TOUCH_MAX_POINTS) {
        n = CONFIG_ESP_LCD_TOUCH_MAX_POINTS;
    }
    if (n > MAX_POINTS) {
        n = MAX_POINTS;
    }
    if (n) {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_rx_param(tp->io, REG_STATUS + 1, &buf[1], n * POINT_SIZE), TAG, "points");
    }
    /* the controller keeps the frame until the status is cleared */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(tp->io, REG_STATUS, &clear, 1), TAG, "clear status");

    taskENTER_CRITICAL(&tp->data.lock);
    for (uint8_t i = 0; i < n; i++) {
        const uint8_t *p = &buf[1 + i * POINT_SIZE];

        np[i] = (esp_lcd_touch_point_data_t){ .track_id = p[0], .x = le16(&p[1]), .y = le16(&p[3]), .strength = le16(&p[5]) };
        /* a resting finger repeats with a pixel or two of noise: keep where it was */
        for (uint8_t j = 0; j < tp->data.points; j++) {
            const esp_lcd_touch_point_data_t *o = &tp->data.coords[j];

            if (o->track_id == np[i].track_id && abs((int)o->x - (int)np[i].x) <= JITTER_PX && abs((int)o->y - (int)np[i].y) <= JITTER_PX) {
                np[i].x = o->x;
                np[i].y = o->y;
                break;
            }
        }
    }
    memcpy(tp->data.coords, np, n * sizeof(np[0]));
    tp->data.points = n;
    taskEXIT_CRITICAL(&tp->data.lock);
    return ESP_OK;
}

static bool get_xy(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y, uint16_t *strength, uint8_t *point_num, uint8_t max_point_num)
{
    taskENTER_CRITICAL(&tp->data.lock);
    uint8_t n = tp->data.points < max_point_num ? tp->data.points : max_point_num;

    for (uint8_t i = 0; i < n; i++) {
        x[i] = tp->data.coords[i].x;
        y[i] = tp->data.coords[i].y;
        if (strength) {
            strength[i] = tp->data.coords[i].strength;
        }
    }
    *point_num = n;
    tp->data.points = tp->data.points;      /* the points stay until the next frame */
    taskEXIT_CRITICAL(&tp->data.lock);
    return n > 0;
}

static esp_err_t del(esp_lcd_touch_handle_t tp)
{
    free(tp);
    return ESP_OK;
}

esp_err_t esp_lcd_touch_new_i2c_gt967(const esp_lcd_panel_io_handle_t io, const esp_lcd_touch_config_t *config, esp_lcd_touch_handle_t *out_touch)
{
    ESP_RETURN_ON_FALSE(io && config && out_touch, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    esp_lcd_touch_handle_t tp = calloc(1, sizeof(*tp));
    uint8_t id[4];
    esp_err_t e;

    ESP_RETURN_ON_FALSE(tp, ESP_ERR_NO_MEM, TAG, "no memory");
    tp->io = io;
    tp->config = *config;
    tp->data.lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    tp->read_data = read_data;
    tp->get_xy = get_xy;
    tp->del = del;
    e = chip_reset(tp);
    if (e == ESP_OK) {
        e = esp_lcd_panel_io_rx_param(io, REG_PRODUCT_ID, id, sizeof(id));
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "no answer from the controller");
        } else if ((((uint32_t)id[0] | id[1] << 8 | (uint32_t)id[2] << 16)) != PRODUCT_ID_967) {
            ESP_LOGE(TAG, "unexpected product id %02x %02x %02x %02x", id[0], id[1], id[2], id[3]);
            e = ESP_ERR_NOT_FOUND;
        }
    }
    if (e == ESP_OK) {
        e = chip_configure(tp);
    }
    if (e != ESP_OK) {
        free(tp);
        return e;
    }
    ESP_LOGI(TAG, "GT%c%c%c ready", id[0], id[1], id[2]);
    *out_touch = tp;
    return ESP_OK;
}
