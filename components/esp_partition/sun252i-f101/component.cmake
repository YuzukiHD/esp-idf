# Partition table access of the sun252i-f101 target: the stock partition list on esp_flash, with
# the table read through the RAM mapping of spi_flash.
idf_component_register(SRCS "partition.c" "sun252i-f101/partition_target_f101.c"
                       INCLUDE_DIRS "../include"
                       PRIV_INCLUDE_DIRS "include" "../../bootloader_support/include"
                       REQUIRES spi_flash esp_blockdev
                       PRIV_REQUIRES esp_system partition_table esp_rom)
