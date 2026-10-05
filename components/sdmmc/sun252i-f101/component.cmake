# Protocol layer of the SD/MMC stack on the sun252i-f101 target: no host-controller
# power control or block device.
set(srcs "sdmmc_cmd.c" "sdmmc_common.c" "sdmmc_init.c" "sdmmc_mmc.c" "sdmmc_sd.c")
if(CONFIG_SD_ENABLE_SDIO_SUPPORT)
    list(APPEND srcs "sdmmc_io.c")
endif()
idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "../include" "../../esp_hal_sd/include" "../../esp_blockdev/include"
                       REQUIRES esp_blockdev soc
                       PRIV_REQUIRES esp_timer)
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/..")
