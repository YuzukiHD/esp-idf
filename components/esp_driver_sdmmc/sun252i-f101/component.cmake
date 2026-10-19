# SD/MMC host of the sun252i-f101 target (SMHC0): the stock sdmmc_host_* API, the pads come from sdmmc_slot_config_t.
idf_component_register(SRCS "sun252i-f101/sdmmc_f101.c"
                       INCLUDE_DIRS "../include" "../legacy/include" "../../esp_hal_sd/include"
                       REQUIRES esp_hw_support esp_rom soc freertos esp_driver_gpio sdmmc
                       PRIV_REQUIRES esp_timer esp_system)
