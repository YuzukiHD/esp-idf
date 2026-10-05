# Display of the sun252i-f101 target: the esp_lcd API with an RGB panel on the display
# engine / TCON pipeline stack (ESP-IDF OS port and board description below).

set(dpy_srcs
    "sun252i-f101/sunxi/core/dpy_graph.c"
    "sun252i-f101/sunxi/core/dpy_device.c"
    "sun252i-f101/sunxi/core/dpy_driver_table.c"
    "sun252i-f101/sunxi/core/dpy_format.c"
    "sun252i-f101/sunxi/core/dpy_objects.c"
    "sun252i-f101/sunxi/core/dpy_atomic.c"
    "sun252i-f101/sunxi/core/dpy_panel.c"
    "sun252i-f101/sunxi/core/dpy_tcon.c"
    "sun252i-f101/sunxi/core/dpy_dsi.c"
    "sun252i-f101/sunxi/core/dpy_pins.c"
    "sun252i-f101/sunxi/core/dpy_debug.c"
    "sun252i-f101/sunxi/api/display_api.c"
    "sun252i-f101/sunxi/display-engine/de_drv.c"
    "sun252i-f101/sunxi/display-engine/de_crtc.c"
    "sun252i-f101/sunxi/display-engine/de_plane.c"
    "sun252i-f101/sunxi/display-engine/de_top.c"
    "sun252i-f101/sunxi/display-engine/de_regs.c"
    "sun252i-f101/sunxi/display-engine/de_rcq.c"
    "sun252i-f101/sunxi/display-engine/de_pipeline.c"
    "sun252i-f101/sunxi/display-engine/de_channel.c"
    "sun252i-f101/sunxi/display-engine/de_disp.c"
    "sun252i-f101/sunxi/display-engine/modules/de_ovl.c"
    "sun252i-f101/sunxi/display-engine/modules/de_scaler_coef.c"
    "sun252i-f101/sunxi/display-engine/modules/de_vsu.c"
    "sun252i-f101/sunxi/display-engine/modules/de_gsu.c"
    "sun252i-f101/sunxi/display-engine/modules/de_csc.c"
    "sun252i-f101/sunxi/display-engine/modules/de_ccsc.c"
    "sun252i-f101/sunxi/display-engine/modules/de_dcsc.c"
    "sun252i-f101/sunxi/display-engine/modules/de_bld.c"
    "sun252i-f101/sunxi/display-engine/modules/de_dither.c"
    "sun252i-f101/sunxi/display-engine/modules/de_gamma.c"
    "sun252i-f101/sunxi/display-engine/soc/de_sun252iw2.c"
    "sun252i-f101/sunxi/tcon/tcon_top.c"
    "sun252i-f101/sunxi/tcon/tcon_lcd.c"
    "sun252i-f101/sunxi/tcon/tcon_clk.c"
    "sun252i-f101/sunxi/encoder/encoder_common.c"
    "sun252i-f101/sunxi/encoder/rgb/rgb.c"
    "sun252i-f101/sunxi/panel/panel_cmdseq.c"
    "sun252i-f101/sunxi/panel/panel_simple.c"
    "sun252i-f101/sunxi/backlight/bl_pwm.c"
    "sun252i-f101/sunxi/soc/soc_sun252iw2.c"
    "sun252i-f101/sunxi/core/dpy_log.c"
)

idf_component_register(SRCS ${dpy_srcs}
                            "sun252i-f101/src/dpy_os_idf.c"
                            "sun252i-f101/src/dpy_board_f101.c"
                            "sun252i-f101/src/esp_lcd_panel_rgb_f101.c"
                            "src/esp_lcd_panel_ops.c"
                       INCLUDE_DIRS "../include" "../interface" "../rgb/include" "../../esp_hal_lcd/include" "../../esp_driver_parlio/include" "../../esp_driver_i2s/include" "../../esp_hal_parlio/include" "../../esp_hal_i2s/include"
                       PRIV_INCLUDE_DIRS "../priv_include" "include" "sunxi" "sunxi/include"
                       REQUIRES soc esp_hw_support hal esp_driver_gpio esp_driver_i2c esp_driver_spi
                       PRIV_REQUIRES freertos heap log esp_timer esp_rom)

# the stack is built for one pipeline: display engine, TCON LCD, RGB output, simple panel, PWM backlight
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    CONFIG_DISPLAY_DE=1
    CONFIG_DISPLAY_TCON_LCD=1
    CONFIG_DISPLAY_ENCODER_RGB=1
    CONFIG_DISPLAY_PANEL_SIMPLE=1
    CONFIG_DISPLAY_BACKLIGHT_PWM=1
    CONFIG_DISPLAY_SOC_SUN252IW2=1)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-parameter -Wno-sign-compare -Wno-missing-field-initializers
                                                -Wno-error=unused-but-set-variable -Wno-error=unused-variable
                                                -Wno-error=unused-function -Wno-error=maybe-uninitialized -Wno-format)
# the public headers of the display API

