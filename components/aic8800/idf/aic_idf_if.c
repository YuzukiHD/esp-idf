/*
 * SPDX-FileCopyrightText: 2026 Yuzuki Tsuru
 * SPDX-License-Identifier: Apache-2.0
 *
 * The AIC8800 as the chip driver of esp_wifi: the seam of aic_zif.h (frames, link events) on one side,
 * f101_wifi_backend_t on the other. Scan and join block in the driver, so they run in a task of their own;
 * IP configuration and DHCP are done by esp_netif on the connected event.
 */
#include <stdlib.h>
#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_private/f101_wifi_backend.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "aic8800.h"
#include "aic_zif.h"
#include "aic_pbuf.h"
#include "plat_config.h"

/* the glue keeps its memory out of the pool of the driver */
#undef malloc
#undef calloc
#undef realloc
#undef aligned_alloc

extern void aic_mem_release(void);

static const char *TAG = "aic8800";

#define MAX_SCAN_APS    32
#define TASK_STACK      6144
#define TASK_PRIO       10

static aic8800_config_t s_cfg;
static int s_zif_token;
static uint8_t s_mac[6];
static uint8_t s_ssid[33];
static uint8_t s_ssid_len;
static wifi_auth_mode_t s_auth;
static wifi_sta_config_t s_join;
static bool s_started;
static bool s_busy;

const aic8800_config_t *aic8800_idf_config(void)
{
    return &s_cfg;
}

/* ---- seam: the IDF side of aic_zif.h ------------------------------------------------------------- */

void *aicz_zif(void)
{
    return &s_zif_token;
}

int aicz_zif_set_mac(void *zif, const uint8_t *mac)
{
    memcpy(s_mac, mac, 6);
    return 0;
}

void aicz_zif_carrier(void *zif, bool on)
{
}

int aicz_zif_rx(void *zif, const void *data, size_t len)
{
    f101_wifi_rx(WIFI_IF_STA, data, (uint16_t)len);
    return 0;
}

void aicz_zif_set_default(void *zif)
{
}

int aicz_zif_dhcp_start(void *zif)
{
    return -1;                      /* esp_netif runs the DHCP client */
}

void aicz_zif_dhcp_stop(void *zif)
{
}

int aicz_zif_ip_assigned(void *zif)
{
    return -1;
}

int aicz_zif_ip_get(void *zif, uint32_t *ip, uint32_t *mask, uint32_t *gw)
{
    return -1;
}

int aicz_zif_ip_set(void *zif, uint32_t ip, uint32_t mask, uint32_t gw)
{
    return 0;
}

void aicz_zif_down(void)
{
}

void aicz_link_event(enum aicz_link_event ev)
{
    struct aicz_sta_info info;

    if (ev == AICZ_LINK_CONNECTED) {
        aicz_wifi_sta_info(&info);
        f101_wifi_sta_connected(info.bssid, info.ssid_len ? info.ssid : s_ssid, info.ssid_len ? info.ssid_len : s_ssid_len,
                                info.channel, s_auth);
    } else {
        static const uint8_t none[6];

        f101_wifi_sta_disconnected(none, s_ssid, s_ssid_len, WIFI_REASON_UNSPECIFIED);
    }
}

/* The static data of the driver is in two ranges of its own (aic_state.ld). The first start takes a copy of the
 * data range, before the driver has changed anything; stopping the chip puts both ranges back. */
extern char _aic_data_start[], _aic_data_end[], _aic_bss_start[], _aic_bss_end[];
static void *s_data_snapshot;

static void state_snapshot(void)
{
    size_t n = _aic_data_end - _aic_data_start;

    if (!s_data_snapshot && n) {
        s_data_snapshot = malloc(n);
        if (s_data_snapshot) {
            memcpy(s_data_snapshot, _aic_data_start, n);
        }
    }
}

void aic_state_reset(void)
{
    memset(_aic_bss_start, 0, _aic_bss_end - _aic_bss_start);
    if (s_data_snapshot) {
        memcpy(_aic_data_start, s_data_snapshot, _aic_data_end - _aic_data_start);
    }
}

/* things of the Zephyr build that have no meaning here: the driver memory is the heap of the system */
void aic_bt_reset_assert(void)
{
}

extern void rtos_al_shutdown(void);
extern void aic_sdio_irq_off(void);

/* ---- backend ------------------------------------------------------------------------------------ */

extern void aic_sdio_card_release(void);

/* every task and timer of the driver removed, the bus released, the chip off, the static data as at the boot */
static void halt_chip(void)
{
    aic_sdio_irq_off();
    vTaskDelay(pdMS_TO_TICKS(100));        /* no task may be in the middle of a bus transfer */
    rtos_al_shutdown();
    aic_sdio_card_release();
    platform_set_regon_en(0);
    aic_state_reset();
    aic_mem_release();
}

static esp_err_t be_start(void)
{
    ESP_RETURN_ON_FALSE(!s_started, ESP_OK, TAG, "already started");
    state_snapshot();
    int ret = aicz_wifi_start();

    if (ret != 0) {
        halt_chip();                    /* leave the driver as it was before, so that the next try starts clean */
        ESP_LOGE(TAG, "the chip did not start (%d)", ret);
        return ESP_FAIL;
    }
    memcpy(s_mac, aicz_wifi_mac(), 6);
    s_started = true;
    return ESP_OK;
}

static esp_err_t be_stop(void)
{
    aicz_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(200));
    halt_chip();
    s_started = false;
    return ESP_OK;
}

static esp_err_t be_get_mac(wifi_interface_t ifx, uint8_t mac[6])
{
    memcpy(mac, s_mac, 6);
    return ESP_OK;
}

static wifi_auth_mode_t auth_of(uint8_t security)
{
    switch (security) {
    case 0:
        return WIFI_AUTH_OPEN;
    case 1:
        return WIFI_AUTH_WEP;
    case 3:
        return WIFI_AUTH_WPA3_PSK;
    default:
        return WIFI_AUTH_WPA2_PSK;
    }
}

typedef struct {
    wifi_ap_record_t *aps;
    uint16_t n;
} scan_acc_t;

/* what the last scan found, to tell the authentication of a network that is joined by its name */
static struct {
    uint8_t ssid[33];
    wifi_auth_mode_t auth;
} s_known[MAX_SCAN_APS];
static int s_known_n;

static wifi_auth_mode_t known_auth(const uint8_t *ssid, wifi_auth_mode_t fallback)
{
    for (int i = 0; i < s_known_n; i++) {
        if (strncmp((const char *)s_known[i].ssid, (const char *)ssid, 32) == 0) {
            return s_known[i].auth;
        }
    }
    return fallback;
}

static void scan_one(const struct aicz_scan_ap *ap, void *arg)
{
    scan_acc_t *a = arg;

    if (a->n >= MAX_SCAN_APS) {
        return;
    }
    wifi_ap_record_t *r = &a->aps[a->n++];

    memset(r, 0, sizeof(*r));
    memcpy(r->ssid, ap->ssid, ap->ssid_len);
    memcpy(r->bssid, ap->bssid, 6);
    r->primary = ap->channel;
    r->rssi = ap->rssi;
    r->authmode = auth_of(ap->security);
    if (s_known_n < MAX_SCAN_APS) {
        memcpy(s_known[s_known_n].ssid, ap->ssid, ap->ssid_len);
        s_known[s_known_n].ssid[ap->ssid_len] = 0;
        s_known[s_known_n].auth = r->authmode;
        s_known_n++;
    }
}

static void scan_task(void *arg)
{
    scan_acc_t acc = { .aps = calloc(MAX_SCAN_APS, sizeof(wifi_ap_record_t)) };

    s_known_n = 0;

    if (acc.aps && aicz_wifi_scan(scan_one, &acc) == 0) {
        f101_wifi_scan_done(acc.aps, acc.n);
    } else {
        f101_wifi_scan_done(NULL, 0);
    }
    free(acc.aps);
    s_busy = false;
    vTaskDelete(NULL);
}

static esp_err_t be_scan(const wifi_scan_config_t *config)
{
    ESP_RETURN_ON_FALSE(!s_busy, ESP_ERR_WIFI_STATE, TAG, "busy");
    s_busy = true;
    if (xTaskCreate(scan_task, "aic_scan", TASK_STACK, NULL, TASK_PRIO, NULL) != pdPASS) {
        s_busy = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void join_task(void *arg)
{
    int ret = aicz_wifi_connect(s_join.ssid, strlen((char *)s_join.ssid), s_join.password[0] ? s_join.password : NULL,
                                strlen((char *)s_join.password), s_join.channel);

    if (ret != 0) {
        static const uint8_t none[6];

        f101_wifi_sta_disconnected(none, s_ssid, s_ssid_len, WIFI_REASON_AUTH_FAIL);
    }
    s_busy = false;
    vTaskDelete(NULL);
}

static esp_err_t be_connect(const wifi_sta_config_t *config)
{
    ESP_RETURN_ON_FALSE(!s_busy, ESP_ERR_WIFI_STATE, TAG, "busy");
    s_join = *config;
    s_ssid_len = strnlen((char *)config->ssid, 32);
    memcpy(s_ssid, config->ssid, s_ssid_len);
    s_auth = known_auth(config->ssid, config->password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN);
    s_busy = true;
    if (xTaskCreate(join_task, "aic_join", TASK_STACK, NULL, TASK_PRIO, NULL) != pdPASS) {
        s_busy = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t be_disconnect(void)
{
    return aicz_wifi_disconnect() == 0 ? ESP_OK : ESP_FAIL;
}

static int be_tx(wifi_interface_t ifx, const void *frame, uint16_t len)
{
    struct pbuf *p = pbuf_alloc(PBUF_RAW, len, PBUF_RAM);

    if (!p) {
        return -1;
    }
    memcpy(p->payload, frame, len);
    return aicz_net_tx(p) == 0 ? len : -1;
}

static esp_err_t be_get_rssi(int *rssi)
{
    struct aicz_sta_info info;

    aicz_wifi_sta_info(&info);
    if (!info.connected) {
        return ESP_FAIL;
    }
    *rssi = info.rssi;
    return ESP_OK;
}

static const f101_wifi_backend_t k_backend = {
    .start = be_start, .stop = be_stop, .get_mac = be_get_mac, .scan = be_scan, .connect = be_connect,
    .disconnect = be_disconnect, .tx = be_tx, .get_rssi = be_get_rssi,
};

esp_err_t aic8800_wifi_register(const aic8800_config_t *config)
{
    ESP_RETURN_ON_FALSE(config && config->reg_on_gpio >= 0, ESP_ERR_INVALID_ARG, TAG, "invalid configuration");
    memset(_aic_bss_start, 0, _aic_bss_end - _aic_bss_start);     /* the start code does not clear this range */
    s_cfg = *config;
    return f101_wifi_register_backend(&k_backend);
}
