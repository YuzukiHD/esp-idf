# LEDC driver of the sun252i-f101 target (PWM controller).
idf_component_register(SRCS "sun252i-f101/ledc_f101.c"
                       INCLUDE_DIRS "../include" "../../esp_hal_ledc/include"
                       REQUIRES esp_hw_support esp_rom soc freertos esp_driver_gpio
                       PRIV_REQUIRES esp_timer esp_system)
