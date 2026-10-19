# AIC8800 WiFi chips on the sun252i-f101 target: the FullMAC host driver and the supplicant (driver/),
# the glue to FreeRTOS, the SDIO host and esp_wifi in idf/.
idf_build_get_property(target IDF_TARGET)

# the requirements have to be known in the early expansion, where the configuration is not yet available
if(CMAKE_BUILD_EARLY_EXPANSION)
    idf_component_register(REQUIRES esp_wifi esp_driver_sdmmc esp_driver_gpio freertos efuse aic8800_socket mbedtls
                           PRIV_REQUIRES esp_timer esp_event log)
    return()
endif()

if(NOT ${target} STREQUAL "sun252i-f101")
    idf_component_register()
    return()
endif()

set(AIC "${CMAKE_CURRENT_LIST_DIR}/driver")
include(${AIC}/sources.cmake)

set(srcs)
foreach(f ${AIC_COMMON_SRCS} ${AIC_DRV_SRCS} ${AIC_DRV_PRIV_SRCS} ${AIC_WPA_SRCS})
    list(APPEND srcs "${AIC}/${f}")
endforeach()
if(CONFIG_AIC8800_P2P)
    foreach(f ${AIC_P2P_SRCS})
        list(APPEND srcs "${AIC}/${f}")
    endforeach()
endif()
if(CONFIG_AIC8800_WPA3)
    foreach(f ${AIC_WPA3_SRCS})
        list(APPEND srcs "${AIC}/${f}")
    endforeach()
endif()
list(APPEND srcs
    "${AIC}/host/net_al/zephyr/net_al.c"
    "idf/net_al_wifi.c"
    "${AIC}/wifi.c" "${AIC}/wifi_monitor.c" "${AIC}/userconfig.c" "${AIC}/powerlimit.c"
    "idf/porting.c" "idf/plat_config.c" "idf/sdio_port.c" "idf/sd_compat.c" "idf/rtos_al.c"
    "idf/aic_log.c" "idf/aic_mem.c" "idf/aic_idf_if.c")

set(incs
    host/inc/priv host/common/api host/drv/macif host/drv/sdio host/drv/fw host/drv/wlan host/drv/fhost host/drv/aic_bsp
    . inc host/net_al/zephyr host/wpa_supplicant/src host/wpa_supplicant/src/utils host/wpa_supplicant/src/rsn_supp
    host/wpa_supplicant/wpa_supplicant host/lbh_release/lbh_lib compat)
set(abs_incs "idf" "include")
foreach(i ${incs})
    list(APPEND abs_incs "${AIC}/${i}")
endforeach()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS ${abs_incs}
                       REQUIRES esp_wifi esp_driver_sdmmc esp_driver_gpio freertos efuse aic8800_socket mbedtls
                       PRIV_REQUIRES esp_timer esp_event log)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    PLATFORM_ALLWIN_FREERTOS CONFIG_RWNX CONFIG_RWNX_FULLMAC USE_5G CONFIG_PMIC_SETTING CONFIG_DOWNLOAD_FW
    CONFIG_LOAD_USERCONFIG CONFIG_OFFCHANNEL CONFIG_RWNX_DBG AIC_LOG_DEBUG_ON CONFIG_FHOST_TX_SCHEDULE_SEPERATE
    CONFIG_FHOST_TX_AC_SCHEDULE CONFIG_TX_NOCOPY OS_NO_C_LIB_DEFINES CONFIG_SDIO_SUPPORT)
if(CONFIG_AIC8800_P2P)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CONFIG_P2P CONFIG_INTERWORKING CONFIG_GAS CONFIG_HS20)
endif()
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu17 -w -fno-strict-aliasing -include ctype.h
    -Wno-error=incompatible-pointer-types -Wno-error=int-conversion -Wno-error=implicit-function-declaration)
target_compile_options(${COMPONENT_LIB} PRIVATE "SHELL:-include ${CMAKE_CURRENT_LIST_DIR}/include/aic_log_redirect.h")
target_compile_options(${COMPONENT_LIB} PRIVATE "SHELL:-include ${CMAKE_CURRENT_LIST_DIR}/include/aic_idf_compat.h")
target_compile_options(${COMPONENT_LIB} PRIVATE "SHELL:-include ${CMAKE_CURRENT_LIST_DIR}/include/aic_rename.h")
target_compile_options(${COMPONENT_LIB} PRIVATE "SHELL:-include ${CMAKE_CURRENT_LIST_DIR}/include/aic_mem_redirect.h")
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=free")
# the crypto library calls the allocator of the mbedtls component, which sits before it on the link line
if(CONFIG_AIC8800_WPA3)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u esp_mbedtls_mem_calloc" "-u esp_mbedtls_mem_free")
endif()
