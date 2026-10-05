# SD/MMC host of the sun252i-f101 target (SMHC0), used with sdmmc_card_init().
idf_component_register(SRCS "sun252i-f101/sdmmc_f101.c"
                       INCLUDE_DIRS "../include" "include" "../../esp_hal_sd/include"
                       REQUIRES esp_hw_support esp_rom soc freertos esp_driver_gpio sdmmc
                       PRIV_REQUIRES esp_timer esp_system)
