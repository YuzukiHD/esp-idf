# General purpose timer driver of the sun252i-f101 target (HSTIMER).
idf_component_register(SRCS "sun252i-f101/timer_f101.c"
                       INCLUDE_DIRS "../include" "../../esp_hal_timg/include"
                       REQUIRES esp_hw_support esp_rom soc freertos
                       PRIV_REQUIRES esp_timer esp_system)
