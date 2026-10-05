/*
 * SPDX-FileCopyrightText: 2026 Yuzuki Tsuru
 * SPDX-License-Identifier: Apache-2.0
 *
 * esp_task_wdt.h: subscribed tasks and users must reset the watchdog inside
 * the timeout. The check runs from an esp_timer callback; the SoC watchdog
 * (reset mode) is fed by it as the backstop, so a stuck system resets even if
 * the check itself no longer runs.
 */

#include <string.h>
#include "sdkconfig.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_freertos_hooks.h"
#include "soc/sun252i_f101_ll.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define WDT_BASE        0x06011000u
#define WDT_IRQ_EN      0x00u
#define WDT_STATUS      0x04u
#define WDT_CTL         0x10u
#define WDT_CFG         0x14u
#define WDT_MODE        0x18u
#define WDT_OUT_CFG     0x1cu
#define WDT_KEY         0x16aa0000u
#define MAX_SUBS        16

static const char *TAG = "task_wdt";

struct esp_task_wdt_user_handle_s {
    const char *name;
    TaskHandle_t task;
    bool used, user, fed;
};

static struct esp_task_wdt_user_handle_s s_sub[MAX_SUBS];
static esp_task_wdt_config_t s_cfg;
static bool s_inited;
static esp_timer_handle_t s_timer;
static int64_t s_last_ok_us;
static bool s_triggered;

/* timeout codes of the hardware: 0.5 1 2 3 4 5 6 8 10 12 14 16 s */
static uint32_t hw_code_for_ms(uint32_t ms)
{
    static const uint16_t t[12] = { 500, 1000, 2000, 3000, 4000, 5000, 6000, 8000, 10000, 12000, 14000, 16000 };

    for (uint32_t i = 0; i < 12; i++) {
        if (t[i] >= ms) {
            return i;
        }
    }
    return 11;
}

static void hw_arm(uint32_t timeout_ms)
{
    F101_REG32(WDT_BASE + WDT_MODE) = WDT_KEY;                 /* off while reprogramming */
    F101_REG32(WDT_BASE + WDT_IRQ_EN) = 0;
    F101_REG32(WDT_BASE + WDT_STATUS) = 1;
    F101_REG32(WDT_BASE + WDT_OUT_CFG) = 0x3f;
    F101_REG32(WDT_BASE + WDT_CFG) = WDT_KEY | 1u;             /* reset the SoC */
    F101_REG32(WDT_BASE + WDT_MODE) = WDT_KEY | (hw_code_for_ms(timeout_ms) << 4) | 1u;
    F101_REG32(WDT_BASE + WDT_CTL) = (0x0a57u << 1) | 1u;
}

static void hw_feed(void)
{
    F101_REG32(WDT_BASE + WDT_CTL) = (0x0a57u << 1) | 1u;
}

static void hw_disarm(void)
{
    F101_REG32(WDT_BASE + WDT_MODE) = WDT_KEY;
}

static bool all_fed(void)
{
    for (int i = 0; i < MAX_SUBS; i++) {
        if (s_sub[i].used && !s_sub[i].fed) {
            return false;
        }
    }
    return true;
}

static struct esp_task_wdt_user_handle_s *find_task(TaskHandle_t t)
{
    for (int i = 0; i < MAX_SUBS; i++) {
        if (s_sub[i].used && !s_sub[i].user && s_sub[i].task == t) {
            return &s_sub[i];
        }
    }
    return NULL;
}

static void report_triggered(task_wdt_msg_handler h, void *opaque)
{
    char line[64];

    for (int i = 0; i < MAX_SUBS; i++) {
        if (s_sub[i].used && !s_sub[i].fed) {
            snprintf(line, sizeof(line), " - %s (CPU 0)", s_sub[i].user ? s_sub[i].name : pcTaskGetName(s_sub[i].task));
            h(opaque, line);
        }
    }
}

static void print_line(void *opaque, const char *msg)
{
    (void)opaque;
    ESP_EARLY_LOGE(TAG, "%s", msg);
}

esp_err_t esp_task_wdt_print_triggered_tasks(task_wdt_msg_handler msg_handler, void *opaque, int *cpus_fail)
{
    ESP_RETURN_ON_FALSE(msg_handler, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    if (all_fed()) {
        return ESP_OK;
    }
    if (cpus_fail) {
        *cpus_fail = 1;
    }
    report_triggered(msg_handler, opaque);
    return ESP_FAIL;
}

void __attribute__((weak)) esp_task_wdt_isr_user_handler(void)
{
}

static void check_cb(void *arg)
{
    int64_t now = esp_timer_get_time();

    if (all_fed()) {
        for (int i = 0; i < MAX_SUBS; i++) {
            s_sub[i].fed = false;
        }
        s_last_ok_us = now;
        s_triggered = false;
        hw_feed();
        return;
    }
    if (now - s_last_ok_us >= (int64_t)s_cfg.timeout_ms * 1000 && !s_triggered) {
        s_triggered = true;
        ESP_EARLY_LOGE(TAG, "Task watchdog got triggered. The following tasks/users did not reset the watchdog in time:");
        esp_task_wdt_print_triggered_tasks(print_line, NULL, NULL);
        esp_task_wdt_isr_user_handler();
        if (s_cfg.trigger_panic) {
            esp_system_abort("Task watchdog got triggered");
        }
        /* not panicking: keep the hardware fed so only the report happens */
        hw_feed();
    } else if (!s_triggered) {
        hw_feed();
    }
}

static bool idle_hook(void)
{
    esp_task_wdt_reset();
    return true;
}

static esp_err_t start(const esp_task_wdt_config_t *cfg)
{
    s_cfg = *cfg;
    if (!s_timer) {
        esp_timer_create_args_t a = { .callback = check_cb, .name = "task_wdt" };
        ESP_RETURN_ON_ERROR(esp_timer_create(&a, &s_timer), TAG, "timer create failed");
    } else {
        esp_timer_stop(s_timer);
    }
    uint32_t period = cfg->timeout_ms / 4;
    s_last_ok_us = esp_timer_get_time();
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(s_timer, (uint64_t)(period ? period : 1) * 1000), TAG, "start failed");
    hw_arm(cfg->timeout_ms * 2 + 1000);
    return ESP_OK;
}

esp_err_t esp_task_wdt_init(const esp_task_wdt_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(cfg && cfg->timeout_ms > 0, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(!s_inited, ESP_ERR_INVALID_STATE, TAG, "already initialized");
    memset(s_sub, 0, sizeof(s_sub));
    ESP_RETURN_ON_ERROR(start(cfg), TAG, "start failed");
    s_inited = true;
    if (cfg->idle_core_mask & 1) {
        TaskHandle_t idle = xTaskGetIdleTaskHandleForCore(0);
        esp_task_wdt_add(idle);
        esp_register_freertos_idle_hook_for_cpu(idle_hook, 0);
    }
    return ESP_OK;
}

esp_err_t esp_task_wdt_reconfigure(const esp_task_wdt_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(cfg && cfg->timeout_ms > 0, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(s_inited, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    return start(cfg);
}

esp_err_t esp_task_wdt_deinit(void)
{
    ESP_RETURN_ON_FALSE(s_inited, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    for (int i = 0; i < MAX_SUBS; i++) {
        if (s_sub[i].used && !s_sub[i].user && s_sub[i].task != xTaskGetIdleTaskHandleForCore(0)) {
            return ESP_ERR_INVALID_STATE;       /* tasks still subscribed */
        }
        if (s_sub[i].used && s_sub[i].user) {
            return ESP_ERR_INVALID_STATE;
        }
    }
    esp_timer_stop(s_timer);
    esp_timer_delete(s_timer);
    s_timer = NULL;
    esp_deregister_freertos_idle_hook_for_cpu(idle_hook, 0);
    hw_disarm();
    memset(s_sub, 0, sizeof(s_sub));
    s_inited = false;
    return ESP_OK;
}

static struct esp_task_wdt_user_handle_s *alloc_sub(void)
{
    for (int i = 0; i < MAX_SUBS; i++) {
        if (!s_sub[i].used) {
            memset(&s_sub[i], 0, sizeof(s_sub[i]));
            s_sub[i].used = true;
            return &s_sub[i];
        }
    }
    return NULL;
}

esp_err_t esp_task_wdt_add(TaskHandle_t task_handle)
{
    ESP_RETURN_ON_FALSE(s_inited, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    TaskHandle_t t = task_handle ? task_handle : xTaskGetCurrentTaskHandle();

    ESP_RETURN_ON_FALSE(!find_task(t), ESP_ERR_INVALID_ARG, TAG, "task already subscribed");
    struct esp_task_wdt_user_handle_s *s = alloc_sub();
    ESP_RETURN_ON_FALSE(s, ESP_ERR_NO_MEM, TAG, "no free slot");
    s->task = t;
    return ESP_OK;
}

esp_err_t esp_task_wdt_add_user(const char *user_name, esp_task_wdt_user_handle_t *user_handle_ret)
{
    ESP_RETURN_ON_FALSE(s_inited, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_FALSE(user_name && user_handle_ret, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    struct esp_task_wdt_user_handle_s *s = alloc_sub();
    ESP_RETURN_ON_FALSE(s, ESP_ERR_NO_MEM, TAG, "no free slot");
    s->user = true;
    s->name = user_name;
    *user_handle_ret = s;
    return ESP_OK;
}

esp_err_t esp_task_wdt_reset(void)
{
    ESP_RETURN_ON_FALSE(s_inited, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    struct esp_task_wdt_user_handle_s *s = find_task(xTaskGetCurrentTaskHandle());

    ESP_RETURN_ON_FALSE(s, ESP_ERR_NOT_FOUND, TAG, "task not subscribed");
    s->fed = true;
    return ESP_OK;
}

esp_err_t esp_task_wdt_reset_user(esp_task_wdt_user_handle_t h)
{
    ESP_RETURN_ON_FALSE(s_inited, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_FALSE(h && h->used && h->user, ESP_ERR_INVALID_ARG, TAG, "invalid handle");
    h->fed = true;
    return ESP_OK;
}

esp_err_t esp_task_wdt_delete(TaskHandle_t task_handle)
{
    ESP_RETURN_ON_FALSE(s_inited, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    struct esp_task_wdt_user_handle_s *s = find_task(task_handle ? task_handle : xTaskGetCurrentTaskHandle());

    ESP_RETURN_ON_FALSE(s, ESP_ERR_NOT_FOUND, TAG, "task not subscribed");
    memset(s, 0, sizeof(*s));
    return ESP_OK;
}

esp_err_t esp_task_wdt_delete_user(esp_task_wdt_user_handle_t h)
{
    ESP_RETURN_ON_FALSE(s_inited, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_FALSE(h && h->used && h->user, ESP_ERR_INVALID_ARG, TAG, "invalid handle");
    memset(h, 0, sizeof(*h));
    return ESP_OK;
}

esp_err_t esp_task_wdt_status(TaskHandle_t task_handle)
{
    ESP_RETURN_ON_FALSE(s_inited, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    return find_task(task_handle ? task_handle : xTaskGetCurrentTaskHandle()) ? ESP_OK : ESP_ERR_NOT_FOUND;
}
