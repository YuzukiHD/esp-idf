# The touch panel interface of esp_lcd (same as the managed component esp_lcd_touch), kept here because the
# managed one requires the legacy "driver" component, which does not exist on this target.
idf_component_register(SRCS "esp_lcd_touch.c" INCLUDE_DIRS "include" REQUIRES esp_driver_gpio esp_lcd freertos)
