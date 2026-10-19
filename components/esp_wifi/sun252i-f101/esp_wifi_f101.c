/*
 * SPDX-FileCopyrightText: 2026 Yuzuki Tsuru
 * SPDX-License-Identifier: Apache-2.0
 *
 * esp_wifi.h for the sun252i-f101 target: station mode with scan, join and leave, on a chip driver that
 * registers itself with f101_wifi_register_backend(). The events are the stock WIFI_EVENT ones and the
 * frames reach the network stack through the stock esp_netif glue (wifi_default.c, wifi_netif.c).
 */
#include <stdlib.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_private/wifi.h"
#include "esp_private/f101_wifi_backend.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "wifi";

ESP_EVENT_DEFINE_BASE(WIFI_EVENT);

#define SCAN_WAIT_MS    30000

typedef struct {
    const f101_wifi_backend_t *backend;
    SemaphoreHandle_t lock;
    SemaphoreHandle_t scan_sem;         /* given when a blocking scan is over */
    bool inited;
    bool started;
    bool scanning;
    wifi_mode_t mode;
    wifi_config_t sta_cfg;
    wifi_ap_record_t *aps;              /* result of the last scan, kept until it is read */
    uint16_t ap_count;
    wifi_rxcb_t rxcb[WIFI_IF_MAX];
    wifi_netstack_buf_ref_cb_t buf_ref;
    wifi_netstack_buf_free_cb_t buf_free;
    bool connected;
    wifi_ap_record_t ap_info;
    wifi_ps_type_t ps;
    wifi_storage_t storage;
} wifi_state_t;

static wifi_state_t s;

#define LOCK()   xSemaphoreTake(s.lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s.lock)

#define NEED_INIT() ESP_RETURN_ON_FALSE(s.inited, ESP_ERR_WIFI_NOT_INIT, TAG, "wifi is not initialized")
#define NEED_START() ESP_RETURN_ON_FALSE(s.started, ESP_ERR_WIFI_NOT_STARTED, TAG, "wifi is not started")

static void post(int32_t id, const void *data, size_t size)
{
    esp_event_post(WIFI_EVENT, id, data, size, portMAX_DELAY);
}

esp_err_t f101_wifi_register_backend(const f101_wifi_backend_t *backend)
{
    ESP_RETURN_ON_FALSE(backend && backend->start && backend->stop && backend->get_mac && backend->tx, ESP_ERR_INVALID_ARG, TAG,
                        "incomplete backend");
    ESP_RETURN_ON_FALSE(!s.inited, ESP_ERR_INVALID_STATE, TAG, "wifi is initialized");
    s.backend = backend;
    return ESP_OK;
}

/* ---- lifecycle -------------------------------------------------------------------------------- */

esp_err_t esp_wifi_init(const wifi_init_config_t *config)
{
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(s.backend, ESP_ERR_NOT_SUPPORTED, TAG, "no WiFi chip driver registered");
    ESP_RETURN_ON_FALSE(!s.inited, ESP_OK, TAG, "already initialized");
    s.lock = xSemaphoreCreateMutex();
    s.scan_sem = xSemaphoreCreateBinary();
    if (!s.lock || !s.scan_sem) {
        return ESP_ERR_NO_MEM;
    }
    s.mode = WIFI_MODE_NULL;
    s.ps = WIFI_PS_NONE;
    s.storage = WIFI_STORAGE_RAM;
    s.inited = true;
    return ESP_OK;
}

esp_err_t esp_wifi_deinit(void)
{
    NEED_INIT();
    ESP_RETURN_ON_FALSE(!s.started, ESP_ERR_WIFI_NOT_STOPPED, TAG, "stop wifi first");
    free(s.aps);
    s.aps = NULL;
    s.ap_count = 0;
    vSemaphoreDelete(s.lock);
    vSemaphoreDelete(s.scan_sem);
    s.inited = false;
    return ESP_OK;
}

esp_err_t esp_wifi_set_mode(wifi_mode_t mode)
{
    NEED_INIT();
    ESP_RETURN_ON_FALSE(mode == WIFI_MODE_NULL || mode == WIFI_MODE_STA, ESP_ERR_NOT_SUPPORTED, TAG, "only station mode is supported");
    s.mode = mode;
    return ESP_OK;
}

esp_err_t esp_wifi_get_mode(wifi_mode_t *mode)
{
    NEED_INIT();
    ESP_RETURN_ON_FALSE(mode, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    *mode = s.mode;
    return ESP_OK;
}

esp_err_t esp_wifi_start(void)
{
    NEED_INIT();
    ESP_RETURN_ON_FALSE(s.mode == WIFI_MODE_STA, ESP_ERR_INVALID_ARG, TAG, "set the station mode first");
    if (s.started) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(s.backend->start(), TAG, "the chip did not start");
    s.started = true;
    post(WIFI_EVENT_STA_START, NULL, 0);
    return ESP_OK;
}

esp_err_t esp_wifi_stop(void)
{
    NEED_INIT();
    if (!s.started) {
        return ESP_OK;
    }
    if (s.connected) {
        s.backend->disconnect();
    }
    esp_err_t err = s.backend->stop();

    s.started = false;
    s.connected = false;
    s.scanning = false;
    post(WIFI_EVENT_STA_STOP, NULL, 0);
    return err;
}

esp_err_t esp_wifi_restore(void)
{
    NEED_INIT();
    memset(&s.sta_cfg, 0, sizeof(s.sta_cfg));
    return ESP_OK;
}

/* ---- configuration ---------------------------------------------------------------------------- */

esp_err_t esp_wifi_set_config(wifi_interface_t interface, wifi_config_t *conf)
{
    NEED_INIT();
    ESP_RETURN_ON_FALSE(conf, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(interface == WIFI_IF_STA, ESP_ERR_NOT_SUPPORTED, TAG, "only the station interface exists");
    s.sta_cfg = *conf;
    return ESP_OK;
}

esp_err_t esp_wifi_get_config(wifi_interface_t interface, wifi_config_t *conf)
{
    NEED_INIT();
    ESP_RETURN_ON_FALSE(conf, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(interface == WIFI_IF_STA, ESP_ERR_NOT_SUPPORTED, TAG, "only the station interface exists");
    *conf = s.sta_cfg;
    return ESP_OK;
}

esp_err_t esp_wifi_set_storage(wifi_storage_t storage)
{
    NEED_INIT();
    s.storage = storage;
    return ESP_OK;
}

esp_err_t esp_wifi_set_ps(wifi_ps_type_t type)
{
    NEED_INIT();
    s.ps = type;
    return ESP_OK;
}

esp_err_t esp_wifi_get_ps(wifi_ps_type_t *type)
{
    NEED_INIT();
    ESP_RETURN_ON_FALSE(type, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    *type = s.ps;
    return ESP_OK;
}

esp_err_t esp_wifi_get_mac(wifi_interface_t ifx, uint8_t mac[6])
{
    NEED_INIT();
    ESP_RETURN_ON_FALSE(mac && ifx == WIFI_IF_STA, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    return s.backend->get_mac(ifx, mac);
}

/* ---- scan ------------------------------------------------------------------------------------- */

esp_err_t esp_wifi_scan_start(const wifi_scan_config_t *config, bool block)
{
    NEED_INIT();
    NEED_START();
    wifi_scan_config_t def = { .show_hidden = false, .scan_type = WIFI_SCAN_TYPE_ACTIVE };

    LOCK();
    if (s.scanning) {
        UNLOCK();
        return ESP_ERR_WIFI_STATE;
    }
    s.scanning = true;
    free(s.aps);
    s.aps = NULL;
    s.ap_count = 0;
    UNLOCK();
    xSemaphoreTake(s.scan_sem, 0);
    esp_err_t err = s.backend->scan(config ? config : &def);

    if (err != ESP_OK) {
        s.scanning = false;
        return err;
    }
    if (block) {
        if (xSemaphoreTake(s.scan_sem, pdMS_TO_TICKS(SCAN_WAIT_MS)) != pdTRUE) {
            if (s.backend->scan_stop) {
                s.backend->scan_stop();
            }
            s.scanning = false;
            return ESP_ERR_TIMEOUT;
        }
    }
    return ESP_OK;
}

esp_err_t esp_wifi_scan_stop(void)
{
    NEED_INIT();
    NEED_START();
    if (s.scanning && s.backend->scan_stop) {
        s.backend->scan_stop();
    }
    s.scanning = false;
    return ESP_OK;
}

esp_err_t esp_wifi_scan_get_ap_num(uint16_t *number)
{
    NEED_INIT();
    ESP_RETURN_ON_FALSE(number, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    *number = s.ap_count;
    return ESP_OK;
}

esp_err_t esp_wifi_scan_get_ap_records(uint16_t *number, wifi_ap_record_t *ap_records)
{
    NEED_INIT();
    ESP_RETURN_ON_FALSE(number && ap_records, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    LOCK();
    uint16_t n = *number < s.ap_count ? *number : s.ap_count;

    if (n) {
        memcpy(ap_records, s.aps, n * sizeof(*ap_records));
    }
    *number = n;
    free(s.aps);                                   /* the list is consumed, like the stock one */
    s.aps = NULL;
    s.ap_count = 0;
    UNLOCK();
    return ESP_OK;
}

esp_err_t esp_wifi_clear_ap_list(void)
{
    NEED_INIT();
    LOCK();
    free(s.aps);
    s.aps = NULL;
    s.ap_count = 0;
    UNLOCK();
    return ESP_OK;
}

void f101_wifi_scan_done(const wifi_ap_record_t *records, uint16_t count)
{
    wifi_event_sta_scan_done_t ev = { .status = 0, .number = count };

    LOCK();
    free(s.aps);
    s.aps = count ? malloc(count * sizeof(*records)) : NULL;
    if (s.aps) {
        memcpy(s.aps, records, count * sizeof(*records));
        s.ap_count = count;
    } else {
        s.ap_count = 0;
        ev.number = 0;
        ev.status = count ? 1 : 0;
    }
    s.scanning = false;
    UNLOCK();
    xSemaphoreGive(s.scan_sem);
    post(WIFI_EVENT_SCAN_DONE, &ev, sizeof(ev));
}

/* ---- join and leave --------------------------------------------------------------------------- */

esp_err_t esp_wifi_connect(void)
{
    NEED_INIT();
    NEED_START();
    ESP_RETURN_ON_FALSE(s.sta_cfg.sta.ssid[0], ESP_ERR_WIFI_SSID, TAG, "no SSID in the station configuration");
    return s.backend->connect(&s.sta_cfg.sta);
}

esp_err_t esp_wifi_disconnect(void)
{
    NEED_INIT();
    NEED_START();
    return s.backend->disconnect();
}

esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap_info)
{
    NEED_INIT();
    ESP_RETURN_ON_FALSE(ap_info, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(s.connected, ESP_ERR_WIFI_NOT_CONNECT, TAG, "not connected");
    *ap_info = s.ap_info;
    if (s.backend->get_rssi) {
        int rssi;

        if (s.backend->get_rssi(&rssi) == ESP_OK) {
            ap_info->rssi = (int8_t)rssi;
        }
    }
    return ESP_OK;
}

void f101_wifi_sta_connected(const uint8_t bssid[6], const uint8_t *ssid, uint8_t ssid_len, uint8_t channel, wifi_auth_mode_t auth)
{
    wifi_event_sta_connected_t ev = { .channel = channel, .authmode = auth };

    ev.ssid_len = ssid_len > sizeof(ev.ssid) ? sizeof(ev.ssid) : ssid_len;
    memcpy(ev.ssid, ssid, ev.ssid_len);
    memcpy(ev.bssid, bssid, 6);
    memset(&s.ap_info, 0, sizeof(s.ap_info));
    memcpy(s.ap_info.bssid, bssid, 6);
    memcpy(s.ap_info.ssid, ev.ssid, ev.ssid_len);
    s.ap_info.primary = channel;
    s.ap_info.authmode = auth;
    s.connected = true;
    post(WIFI_EVENT_STA_CONNECTED, &ev, sizeof(ev));
}

void f101_wifi_sta_disconnected(const uint8_t bssid[6], const uint8_t *ssid, uint8_t ssid_len, wifi_err_reason_t reason)
{
    wifi_event_sta_disconnected_t ev = { .reason = reason };

    ev.ssid_len = ssid_len > sizeof(ev.ssid) ? sizeof(ev.ssid) : ssid_len;
    if (ssid) {
        memcpy(ev.ssid, ssid, ev.ssid_len);
    }
    if (bssid) {
        memcpy(ev.bssid, bssid, 6);
    }
    s.connected = false;
    post(WIFI_EVENT_STA_DISCONNECTED, &ev, sizeof(ev));
}

/* ---- the network stack side ------------------------------------------------------------------- */

esp_err_t esp_wifi_internal_reg_netstack_buf_cb(wifi_netstack_buf_ref_cb_t ref, wifi_netstack_buf_free_cb_t free_cb)
{
    s.buf_ref = ref;
    s.buf_free = free_cb;
    return ESP_OK;
}

esp_err_t esp_wifi_internal_reg_rxcb(wifi_interface_t ifx, wifi_rxcb_t fn)
{
    ESP_RETURN_ON_FALSE(ifx < WIFI_IF_MAX, ESP_ERR_INVALID_ARG, TAG, "invalid interface");
    s.rxcb[ifx] = fn;
    return ESP_OK;
}

esp_err_t esp_wifi_internal_set_sta_ip(void)
{
    return ESP_OK;
}

int esp_wifi_internal_tx(wifi_interface_t ifx, void *buffer, uint16_t len)
{
    if (!s.started || ifx != WIFI_IF_STA || !s.connected) {
        return ESP_ERR_WIFI_IF;
    }
    return s.backend->tx(ifx, buffer, len) < 0 ? ESP_FAIL : ESP_OK;
}

esp_err_t esp_wifi_internal_tx_by_ref(wifi_interface_t ifx, void *buffer, size_t len, void *netstack_buf)
{
    return esp_wifi_internal_tx(ifx, buffer, (uint16_t)len);
}

void esp_wifi_internal_free_rx_buffer(void *buffer)
{
    free(buffer);
}

void f101_wifi_rx(wifi_interface_t ifx, const void *frame, uint16_t len)
{
    if (ifx >= WIFI_IF_MAX || !s.rxcb[ifx] || !len) {
        return;
    }
    void *copy = malloc(len);

    if (!copy) {
        return;
    }
    memcpy(copy, frame, len);
    s.rxcb[ifx](copy, len, copy);                  /* the network stack frees the buffer, also when it drops the frame */
}
