# ADC oneshot driver of the sun252i-f101 target (GPADC).
idf_component_register(SRCS "sun252i-f101/adc_f101.c"
                       INCLUDE_DIRS "../include" "include" "../../esp_hal_ana_conv/include"
                       REQUIRES esp_hw_support esp_rom soc freertos esp_driver_gpio
                       PRIV_REQUIRES esp_timer esp_system)
