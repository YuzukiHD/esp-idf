# The JPEG decoder API on the video engine of the sun252i-f101 target (baseline, 4:2:0).
idf_component_register(SRCS "sun252i-f101/jpeg_ve_f101.c"
                            "sun252i-f101/ve/ve_hw.c"
                            "sun252i-f101/ve/ve_mem.c"
                            "sun252i-f101/ve/ve_pool.c"
                            "sun252i-f101/ve/ve_stream.c"
                            "sun252i-f101/ve/ve_session.c"
                            "sun252i-f101/ve/ve_jpeg.c"
                            "sun252i-f101/ve/ve_jpeg_hw.c"
                            "sun252i-f101/ve/ve_jpeg_parse.c"
                       INCLUDE_DIRS "../include" "../../esp_hal_jpeg/include"
                       REQUIRES soc hal esp_hw_support esp_hal_jpeg
                       PRIV_REQUIRES freertos heap log)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-parameter -Wno-sign-compare -Wno-missing-field-initializers
                                                -Wno-unused-variable -Wno-unused-function -Wno-format -Wno-error=unused-but-set-variable)
