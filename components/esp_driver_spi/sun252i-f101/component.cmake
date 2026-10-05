# SPI master driver of the sun252i-f101 target (SPI0/SPI1 controllers).
idf_component_register(SRCS "sun252i-f101/spi_f101.c"
                       INCLUDE_DIRS "../include" "../../esp_hal_gpspi/include"
                       REQUIRES esp_hw_support esp_rom soc freertos esp_driver_gpio
                       PRIV_REQUIRES esp_timer esp_system)
