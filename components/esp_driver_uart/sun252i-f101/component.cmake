# UART driver of the sun252i-f101 target: the stock public API on the DW-APB UARTs.
idf_component_register(SRCS "sun252i-f101/uart_f101.c"
                       INCLUDE_DIRS "../include" "../../esp_hal_uart/include"
                       REQUIRES esp_hw_support esp_rom soc freertos esp_driver_gpio
                       PRIV_REQUIRES esp_timer esp_system)
