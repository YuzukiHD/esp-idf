/*
 * SPDX-FileCopyrightText: 2026 Yuzuki Tsuru
 * SPDX-License-Identifier: Apache-2.0
 *
 * driver/gptimer.h on the two high speed timers (56 bit down counters on a
 * 200 MHz bus clock). The counter seen by the user is an up counter in
 * resolution_hz ticks: the driver keeps the base count and converts the
 * elapsed hardware ticks.
 */

#include <stdlib.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_intr_alloc.h"
#include "driver/gptimer.h"
#include "soc/sun252i_f101_ll.h"
#include "freertos/FreeRTOS.h"

#define HST_BASE        0x03008000u
#define HST_BGR         (F101_CCU_BASE + 0x73cu)
#define HST_IRQ_EN      0x00u
#define HST_IRQ_ST      0x04u
#define HST_CTL(i)      (0x20u + 0x20u * (i))
#define HST_INTVAL_LO(i) (0x24u + 0x20u * (i))
#define HST_INTVAL_HI(i) (0x28u + 0x20u * (i))
#define HST_CNT_LO(i)   (0x2cu + 0x20u * (i))
#define HST_CNT_HI(i)   (0x30u + 0x20u * (i))
#define HST_PLIC_SRC(i) (55 + (i))
#define HST_HZ          200000000ull      /* timer clock = the 200 MHz bus clock */
#define CTL_EN          (1u << 0)
#define CTL_RELOAD      (1u << 1)
#define CTL_ONESHOT     (1u << 7)
#define MAX_TICKS       ((1ull << 56) - 1)

static const char *TAG = "gptimer";

struct gptimer_t {
    int id;
    uint32_t res_hz;
    bool enabled, running;
    bool alarm_set, auto_reload, hw_done;
    uint64_t alarm_count, reload_count;
    uint64_t base;               /* count at the moment the hardware was (re)started */
    uint64_t hw_interval;        /* hardware ticks of the running interval */
    gptimer_alarm_cb_t cb;
    void *user;
    intr_handle_t intr;
};

static struct gptimer_t *s_timer[2];

static inline uint32_t rd(uint32_t off)
{
    return F101_REG32(HST_BASE + off);
}

static inline void wr(uint32_t off, uint32_t v)
{
    F101_REG32(HST_BASE + off) = v;
}

static uint64_t hw_count(int i)
{
    uint32_t hi, lo, hi2;

    do {
        hi = rd(HST_CNT_HI(i)) & 0xffffffu;
        lo = rd(HST_CNT_LO(i));
        hi2 = rd(HST_CNT_HI(i)) & 0xffffffu;
    } while (hi != hi2);
    return ((uint64_t)hi << 32) | lo;
}

static uint64_t to_hw(struct gptimer_t *t, uint64_t ticks)
{
    return ticks / t->res_hz * HST_HZ + (ticks % t->res_hz) * HST_HZ / t->res_hz;
}

static uint64_t from_hw(struct gptimer_t *t, uint64_t hw)
{
    return hw / HST_HZ * t->res_hz + (hw % HST_HZ) * t->res_hz / HST_HZ;
}

static void hw_stop(struct gptimer_t *t)
{
    wr(HST_CTL(t->id), rd(HST_CTL(t->id)) & ~CTL_EN);
}

static void hw_start(struct gptimer_t *t)
{
    uint64_t iv = MAX_TICKS;
    bool periodic = false;

    if (t->alarm_set && t->alarm_count > t->base) {
        iv = to_hw(t, t->alarm_count - t->base);
        if (iv == 0) {
            iv = 1;
        }
        if (iv > MAX_TICKS) {
            iv = MAX_TICKS;
        }
        periodic = t->auto_reload;
    }
    t->hw_interval = iv;
    t->hw_done = false;
    hw_stop(t);
    wr(HST_INTVAL_LO(t->id), (uint32_t)iv);
    wr(HST_INTVAL_HI(t->id), (uint32_t)(iv >> 32));
    uint32_t ctl = (rd(HST_CTL(t->id)) & ~CTL_ONESHOT) | CTL_EN | CTL_RELOAD;
    if (!periodic) {
        ctl |= CTL_ONESHOT;
    }
    wr(HST_CTL(t->id), ctl);
}

static void timer_isr(void *arg)
{
    struct gptimer_t *t = arg;
    BaseType_t woken = pdFALSE;

    if (!(rd(HST_IRQ_ST) & (1u << t->id))) {
        return;
    }
    wr(HST_IRQ_ST, 1u << t->id);
    gptimer_alarm_event_data_t ev = { .count_value = t->alarm_count, .alarm_value = t->alarm_count };

    if (t->auto_reload) {
        /* hardware period = alarm - reload from now on */
        t->base = t->reload_count;
    } else {
        t->base = t->alarm_count;      /* the counter would go on, the hardware has stopped */
        t->hw_done = true;
        t->alarm_set = false;
    }
    if (t->cb && t->cb(t, &ev, t->user)) {
        woken = pdTRUE;
    }
    portYIELD_FROM_ISR(woken);
}

esp_err_t gptimer_new_timer(const gptimer_config_t *cfg, gptimer_handle_t *ret)
{
    ESP_RETURN_ON_FALSE(cfg && ret, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(cfg->resolution_hz > 0 && cfg->resolution_hz <= HST_HZ, ESP_ERR_INVALID_ARG, TAG,
                        "invalid resolution");
    ESP_RETURN_ON_FALSE(cfg->direction == GPTIMER_COUNT_UP, ESP_ERR_NOT_SUPPORTED, TAG, "count up only");
    int id = -1;

    for (int i = 0; i < 2; i++) {
        if (!s_timer[i]) {
            id = i;
            break;
        }
    }
    ESP_RETURN_ON_FALSE(id >= 0, ESP_ERR_NOT_FOUND, TAG, "no free timer");
    struct gptimer_t *t = calloc(1, sizeof(*t));
    ESP_RETURN_ON_FALSE(t, ESP_ERR_NO_MEM, TAG, "no memory");
    t->id = id;
    t->res_hz = cfg->resolution_hz;

    F101_REG32(HST_BGR) &= ~(1u << 16);
    F101_REG32(HST_BGR) |= 1u;
    F101_REG32(HST_BGR) |= (1u << 16);
    wr(HST_CTL(id), 0);
    s_timer[id] = t;
    *ret = t;
    return ESP_OK;
}

esp_err_t gptimer_del_timer(gptimer_handle_t t)
{
    ESP_RETURN_ON_FALSE(t, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(!t->enabled, ESP_ERR_INVALID_STATE, TAG, "timer not disabled");
    s_timer[t->id] = NULL;
    free(t);
    return ESP_OK;
}

esp_err_t gptimer_set_raw_count(gptimer_handle_t t, uint64_t value)
{
    ESP_RETURN_ON_FALSE(t, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    t->base = value;
    if (t->running) {
        hw_start(t);
    }
    return ESP_OK;
}

esp_err_t gptimer_get_raw_count(gptimer_handle_t t, uint64_t *value)
{
    ESP_RETURN_ON_FALSE(t && value, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    if (t->running && !t->hw_done) {
        uint64_t left = hw_count(t->id);
        uint64_t elapsed = t->hw_interval > left ? t->hw_interval - left : 0;

        *value = t->base + from_hw(t, elapsed);
    } else {
        *value = t->base;
    }
    return ESP_OK;
}

esp_err_t gptimer_get_captured_count(gptimer_handle_t t, uint64_t *value)
{
    return gptimer_get_raw_count(t, value);
}

esp_err_t gptimer_get_resolution(gptimer_handle_t t, uint32_t *out)
{
    ESP_RETURN_ON_FALSE(t && out, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    *out = t->res_hz;
    return ESP_OK;
}

esp_err_t gptimer_register_event_callbacks(gptimer_handle_t t, const gptimer_event_callbacks_t *cbs, void *user)
{
    ESP_RETURN_ON_FALSE(t && cbs, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(!t->enabled, ESP_ERR_INVALID_STATE, TAG, "timer already enabled");
    t->cb = cbs->on_alarm;
    t->user = user;
    return ESP_OK;
}

esp_err_t gptimer_set_alarm_action(gptimer_handle_t t, const gptimer_alarm_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(t, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    if (!cfg) {
        t->alarm_set = false;
    } else {
        t->alarm_set = true;
        t->alarm_count = cfg->alarm_count;
        t->reload_count = cfg->reload_count;
        t->auto_reload = cfg->flags.auto_reload_on_alarm;
        ESP_RETURN_ON_FALSE(!t->auto_reload || cfg->alarm_count > cfg->reload_count, ESP_ERR_INVALID_ARG, TAG,
                            "alarm must be above the reload value");
    }
    if (t->running) {
        uint64_t now;
        gptimer_get_raw_count(t, &now);
        t->base = now;
        hw_start(t);
    }
    return ESP_OK;
}

esp_err_t gptimer_enable(gptimer_handle_t t)
{
    ESP_RETURN_ON_FALSE(t, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(!t->enabled, ESP_ERR_INVALID_STATE, TAG, "timer already enabled");
    ESP_RETURN_ON_ERROR(esp_intr_alloc(HST_PLIC_SRC(t->id), ESP_INTR_FLAG_LEVEL1, timer_isr, t, &t->intr), TAG,
                        "intr alloc failed");
    wr(HST_IRQ_ST, 1u << t->id);
    wr(HST_IRQ_EN, rd(HST_IRQ_EN) | (1u << t->id));
    t->enabled = true;
    return ESP_OK;
}

esp_err_t gptimer_disable(gptimer_handle_t t)
{
    ESP_RETURN_ON_FALSE(t, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(t->enabled, ESP_ERR_INVALID_STATE, TAG, "timer not enabled");
    ESP_RETURN_ON_FALSE(!t->running, ESP_ERR_INVALID_STATE, TAG, "timer still running");
    wr(HST_IRQ_EN, rd(HST_IRQ_EN) & ~(1u << t->id));
    esp_intr_free(t->intr);
    t->intr = NULL;
    t->enabled = false;
    return ESP_OK;
}

esp_err_t gptimer_start(gptimer_handle_t t)
{
    ESP_RETURN_ON_FALSE(t, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(t->enabled, ESP_ERR_INVALID_STATE, TAG, "timer not enabled");
    ESP_RETURN_ON_FALSE(!t->running, ESP_ERR_INVALID_STATE, TAG, "timer already running");
    t->running = true;
    hw_start(t);
    return ESP_OK;
}

esp_err_t gptimer_stop(gptimer_handle_t t)
{
    ESP_RETURN_ON_FALSE(t, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(t->running, ESP_ERR_INVALID_STATE, TAG, "timer not running");
    uint64_t now;
    gptimer_get_raw_count(t, &now);
    hw_stop(t);
    t->base = now;
    t->running = false;
    return ESP_OK;
}
