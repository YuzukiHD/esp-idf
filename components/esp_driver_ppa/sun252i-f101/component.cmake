# The pixel processing accelerator API on the 2D accelerator of the sun252i-f101 target.
idf_component_register(SRCS "sun252i-f101/ppa_f101.c"
                            "sun252i-f101/g2d/g2d_sunxi_cmdlist.c"
                            "sun252i-f101/g2d/g2d_sunxi_compose.c"
                            "sun252i-f101/g2d/g2d_sunxi_format.c"
                            "sun252i-f101/g2d/g2d_sunxi_tables.c"
                       INCLUDE_DIRS "../include" "../../esp_hal_ppa/include"
                       REQUIRES soc hal esp_hw_support esp_hal_ppa
                       PRIV_REQUIRES freertos heap log)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-parameter -Wno-sign-compare -Wno-missing-field-initializers
                                                -Wno-unused-variable -Wno-unused-function -Wno-format)
