# GPIO driver of the sun252i-f101 target: the stock public API on the PIO banks.
idf_component_register(SRCS "sun252i-f101/gpio_f101.c"
                       INCLUDE_DIRS "../include" "include" "../../esp_hal_gpio/include"
                       REQUIRES esp_hw_support esp_rom soc freertos
                       PRIV_REQUIRES esp_timer esp_system)
