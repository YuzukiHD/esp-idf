# DMA of the sun252i-f101 target: the linked-list DMA controller (esp_private/dma_f101.h) and
# esp_async_memcpy() on top of it.
idf_component_register(SRCS "sun252i-f101/dma_f101.c" "sun252i-f101/async_memcpy_f101.c"
                       INCLUDE_DIRS "../include" "include"
                       REQUIRES soc esp_hw_support esp_rom heap freertos
                       PRIV_REQUIRES log)
