/*
 * SPDX-FileCopyrightText: 2026 Yuzuki Tsuru
 * SPDX-License-Identifier: Apache-2.0
 *
 * What a WiFi chip driver provides to the esp_wifi API of the sun252i-f101 target. The API
 * (esp_wifi.h, events on WIFI_EVENT, the esp_netif glue of wifi_default.c / wifi_netif.c) is the stock one;
 * the chip sits behind this table. The calls of the table may block, they run in the context of the caller.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "esp_wifi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /** Power the chip up and make it ready (firmware download, interfaces); called by esp_wifi_start(). */
    esp_err_t (*start)(void);
    /** Leave the chip powered down; called by esp_wifi_stop(). */
    esp_err_t (*stop)(void);
    /** MAC address of an interface. */
    esp_err_t (*get_mac)(wifi_interface_t ifx, uint8_t mac[6]);
    /** Start a scan; the result is delivered with f101_wifi_scan_done(). */
    esp_err_t (*scan)(const wifi_scan_config_t *config);
    esp_err_t (*scan_stop)(void);
    /** Join a network; the outcome is delivered with f101_wifi_sta_connected() / f101_wifi_sta_disconnected(). */
    esp_err_t (*connect)(const wifi_sta_config_t *config);
    esp_err_t (*disconnect)(void);
    /** Send an Ethernet frame of an interface. */
    int (*tx)(wifi_interface_t ifx, const void *frame, uint16_t len);
    /** Optional: signal strength of the joined network in dBm. */
    esp_err_t (*get_rssi)(int *rssi);
} f101_wifi_backend_t;

/** Choose the chip driver; call before esp_wifi_init(). */
esp_err_t f101_wifi_register_backend(const f101_wifi_backend_t *backend);

/* ---- driver to API ------------------------------------------------------------------------------ */

/** A scan is over; the records are copied. */
void f101_wifi_scan_done(const wifi_ap_record_t *records, uint16_t count);

void f101_wifi_sta_connected(const uint8_t bssid[6], const uint8_t *ssid, uint8_t ssid_len, uint8_t channel, wifi_auth_mode_t auth);

void f101_wifi_sta_disconnected(const uint8_t bssid[6], const uint8_t *ssid, uint8_t ssid_len, wifi_err_reason_t reason);

/** A received Ethernet frame; the data is copied and handed to the network stack. */
void f101_wifi_rx(wifi_interface_t ifx, const void *frame, uint16_t len);

#ifdef __cplusplus
}
#endif
