# NOR flash of the sun252i-f101 target: the stock esp_flash layer and chip drivers on a host
# driver built on driver/spi_master.h (SPI controller 0, the board flash), flash mapping by RAM copy.
idf_component_register(SRCS "esp_flash_api.c"
                            "spi_flash_chip_drivers.c"
                            "spi_flash_chip_generic.c"
                            "spi_flash_chip_gd.c"
                            "spi_flash_chip_winbond.c"
                            "spi_flash_chip_issi.c"
                            "spi_flash_chip_mxic.c"
                            "spi_flash_chip_boya.c"
                            "spi_flash_chip_th.c"
                            "sun252i-f101/spi_flash_host_f101.c"
                            "sun252i-f101/flash_mmap_f101.c"
                       INCLUDE_DIRS "include" "../include" "../../esp_hal_mspi/include"
                       REQUIRES hal soc esp_blockdev esp_hw_support esp_rom esp_driver_spi esp_driver_gpio freertos
                       PRIV_REQUIRES esp_timer esp_system heap)
