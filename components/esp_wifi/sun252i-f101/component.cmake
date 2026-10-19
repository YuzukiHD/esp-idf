# esp_wifi of the sun252i-f101 target: the stock esp_wifi.h API on a WiFi chip driver that registers a
# f101_wifi_backend_t; the esp_netif glue (wifi_default.c, wifi_netif.c) is the stock one.
idf_component_register(SRCS "sun252i-f101/esp_wifi_f101.c"
                            "src/wifi_default.c"
                            "src/wifi_netif.c"
                       INCLUDE_DIRS "../include" "include"
                       REQUIRES esp_event esp_netif
                       PRIV_REQUIRES esp_system freertos log)
