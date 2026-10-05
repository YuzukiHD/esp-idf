/*
 * SPDX-FileCopyrightText: 2026 Yuzuki Tsuru
 * SPDX-License-Identifier: Apache-2.0
 *
 * Chip identity: esp_chip_info() and the MAC addresses. The SoC has no MAC
 * fuse, so the default MAC is derived from the 128 bit chip id: a hash of it
 * with the locally administered bit set and the multicast bit cleared.
 */

#include <string.h>
#include "esp_err.h"
#include "esp_mac.h"
#include "esp_chip_info.h"
#include "soc/sun252i_f101_ll.h"

static uint8_t s_base_mac[6];
static bool s_base_mac_set;

void esp_chip_info(esp_chip_info_t *out_info)
{
    memset(out_info, 0, sizeof(*out_info));
    out_info->model = CHIP_SUN252I_F101;
    out_info->features = CHIP_FEATURE_EMB_PSRAM;
    out_info->revision = 0;
    out_info->cores = 1;
}

esp_err_t esp_efuse_mac_get_default(uint8_t *mac)
{
    if (!mac) {
        return ESP_ERR_INVALID_ARG;
    }
    uint64_t h = 0xcbf29ce484222325ull;             /* FNV-1a over the chip id */

    for (unsigned int w = 0; w < 4; w++) {
        uint32_t v = f101_sid_read_word(w);
        for (int i = 0; i < 4; i++) {
            h ^= (v >> (8 * i)) & 0xff;
            h *= 0x100000001b3ull;
        }
    }
    for (int i = 0; i < 6; i++) {
        mac[i] = (uint8_t)(h >> (8 * i));
    }
    mac[0] = (mac[0] & 0xfe) | 0x02;
    return ESP_OK;
}

esp_err_t esp_efuse_mac_get_custom(uint8_t *mac)
{
    return ESP_ERR_NOT_SUPPORTED;                   /* no custom MAC fuse */
}

esp_err_t esp_base_mac_addr_set(const uint8_t *mac)
{
    if (!mac || (mac[0] & 1)) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(s_base_mac, mac, 6);
    s_base_mac_set = true;
    return ESP_OK;
}

esp_err_t esp_base_mac_addr_get(uint8_t *mac)
{
    if (!mac) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_base_mac_set) {
        return esp_efuse_mac_get_default(mac);
    }
    memcpy(mac, s_base_mac, 6);
    return ESP_OK;
}

esp_err_t esp_derive_local_mac(uint8_t *local_mac, const uint8_t *universal_mac)
{
    if (!local_mac || !universal_mac) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(local_mac, universal_mac, 6);
    local_mac[0] |= 0x02;
    if (memcmp(local_mac, universal_mac, 6) == 0) {
        local_mac[0] ^= 0x04;
    }
    return ESP_OK;
}

esp_err_t esp_iface_mac_addr_set(const uint8_t *mac, esp_mac_type_t type)
{
    return type == ESP_MAC_BASE ? esp_base_mac_addr_set(mac) : ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esp_read_mac(uint8_t *mac, esp_mac_type_t type)
{
    if (!mac) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = esp_base_mac_addr_get(mac);

    if (err != ESP_OK) {
        return err;
    }
    switch (type) {
    case ESP_MAC_WIFI_STA:
    case ESP_MAC_BASE:
    case ESP_MAC_EFUSE_FACTORY:
        return ESP_OK;
    case ESP_MAC_WIFI_SOFTAP: {
        uint8_t sta[6];

        memcpy(sta, mac, 6);
        return esp_derive_local_mac(mac, sta);
    }
    case ESP_MAC_BT:
        mac[5] += 2;
        return ESP_OK;
    case ESP_MAC_ETH:
        mac[5] += 3;
        return ESP_OK;
    default:
        return ESP_ERR_INVALID_ARG;
    }
}
