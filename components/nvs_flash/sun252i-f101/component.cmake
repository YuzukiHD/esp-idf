# NVS of the sun252i-f101 target: the stock storage on esp_partition; encrypted partitions are
# refused (the XTS entry points are stubs).
idf_component_register(SRCS "src/nvs_api.cpp"
                            "src/nvs_cxx_api.cpp"
                            "src/nvs_item_hash_list.cpp"
                            "src/nvs_page.cpp"
                            "src/nvs_pagemanager.cpp"
                            "src/nvs_storage.cpp"
                            "src/nvs_handle_simple.cpp"
                            "src/nvs_handle_locked.cpp"
                            "src/nvs_partition.cpp"
                            "src/nvs_partition_lookup.cpp"
                            "src/nvs_partition_manager.cpp"
                            "src/nvs_types.cpp"
                            "src/nvs_platform.cpp"
                            "src/nvs_encrypted_partition.cpp"
                            "sun252i-f101/nvs_xts_stub_f101.c"
                       REQUIRES esp_partition esp_blockdev
                       PRIV_REQUIRES spi_flash esp_libc esp_rom
                       INCLUDE_DIRS "../include"
                       PRIV_INCLUDE_DIRS "../private_include" "include")
