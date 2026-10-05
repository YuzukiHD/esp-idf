# I2S of the sun252i-f101 target: driver/i2s_std.h on the on-chip audio codec (I2S_NUM_0), cyclic
# DMA of the linked-list DMA controller, PLL_AUDIO1 and the codec clocks.
idf_component_register(SRCS "sun252i-f101/i2s_f101.c" "sun252i-f101/codec_f101.c"
                       INCLUDE_DIRS "../include" "include" "../../esp_hal_i2s/include"
                       REQUIRES soc hal esp_hw_support esp_driver_gpio esp_driver_dma freertos
                       PRIV_REQUIRES esp_rom)
