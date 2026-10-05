/*
 * SPDX-FileCopyrightText: 2015-2025 Espressif Systems (Shanghai) CO LTD
 * SPDX-FileCopyrightText: 2026 Yuzuki Tsuru
 * SPDX-License-Identifier: Apache-2.0
 *
 * Partition access on top of esp_flash for the sun252i-f101 target: no flash
 * encryption, no application image to protect, no OTA.
 */
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <sys/lock.h>
#include "sdkconfig.h"
#include "esp_flash_partitions.h"
#include "esp_attr.h"
#include "esp_flash.h"
#include "esp_partition.h"
#include "esp_log.h"
#include "spi_flash_mmap.h"

#define HASH_LEN 32 /* SHA-256 digest length */

esp_err_t esp_partition_read(const esp_partition_t *partition,
                             size_t src_offset, void *dst, size_t size)
{
    assert(partition != NULL);
    if (src_offset > partition->size) {
        return ESP_ERR_INVALID_ARG;
    }
    if (size > partition->size - src_offset) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (!partition->encrypted) {
        return esp_flash_read(partition->flash_chip, dst, partition->address + src_offset, size);
    }

#if CONFIG_SPI_FLASH_ENABLE_ENCRYPTED_READ_WRITE
    if (partition->flash_chip != esp_flash_default_chip) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* Encrypted partitions need to be read via a cache mapping */
    const void *buf;
    esp_partition_mmap_handle_t handle;

    esp_err_t err = esp_partition_mmap(partition, src_offset, size,
                                       ESP_PARTITION_MMAP_DATA | ESP_PARTITION_MMAP_BLOCKS_WRITE, &buf, &handle);
    if (err != ESP_OK) {
        return err;
    }
    memcpy(dst, buf, size);
    esp_partition_munmap(handle);
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif // CONFIG_SPI_FLASH_ENABLE_ENCRYPTED_READ_WRITE
}

esp_err_t esp_partition_write(const esp_partition_t *partition,
                              size_t dst_offset, const void *src, size_t size)
{
    assert(partition != NULL);
    if (partition->readonly) {
        return ESP_ERR_NOT_ALLOWED;
    }
    if (dst_offset > partition->size) {
        return ESP_ERR_INVALID_ARG;
    }
    if (size > partition->size - dst_offset) {
        return ESP_ERR_INVALID_SIZE;
    }
    dst_offset = partition->address + dst_offset;
    if (!partition->encrypted) {
        return esp_flash_write(partition->flash_chip, src, dst_offset, size);
    }

#if CONFIG_SPI_FLASH_ENABLE_ENCRYPTED_READ_WRITE
    if (partition->flash_chip != esp_flash_default_chip) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return esp_flash_write_encrypted(partition->flash_chip, dst_offset, src, size);
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif // CONFIG_SPI_FLASH_ENABLE_ENCRYPTED_READ_WRITE
}

esp_err_t esp_partition_read_raw(const esp_partition_t *partition,
                                 size_t src_offset, void *dst, size_t size)
{
    assert(partition != NULL);
    if (src_offset > partition->size) {
        return ESP_ERR_INVALID_ARG;
    }
    if (size > partition->size - src_offset) {
        return ESP_ERR_INVALID_SIZE;
    }

    return esp_flash_read(partition->flash_chip, dst, partition->address + src_offset, size);
}

esp_err_t esp_partition_write_raw(const esp_partition_t *partition,
                                  size_t dst_offset, const void *src, size_t size)
{
    assert(partition != NULL);
    if (partition->readonly) {
        return ESP_ERR_NOT_ALLOWED;
    }
    if (dst_offset > partition->size) {
        return ESP_ERR_INVALID_ARG;
    }
    if (size > partition->size - dst_offset) {
        return ESP_ERR_INVALID_SIZE;
    }
    dst_offset = partition->address + dst_offset;

    return esp_flash_write(partition->flash_chip, src, dst_offset, size);
}

esp_err_t esp_partition_erase_range(const esp_partition_t *partition,
                                    size_t offset, size_t size)
{
    assert(partition != NULL);
    if (partition->readonly) {
        return ESP_ERR_NOT_ALLOWED;
    }
    if (offset > partition->size) {
        return ESP_ERR_INVALID_ARG;
    }
    if (size > partition->size - offset) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (size % SPI_FLASH_SEC_SIZE != 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (offset % SPI_FLASH_SEC_SIZE != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    return esp_flash_erase_region(partition->flash_chip, partition->address + offset, size);
}


esp_err_t esp_partition_mmap(const esp_partition_t *partition, size_t offset, size_t size,
                             esp_partition_mmap_flag_t flags,
                             const void **out_ptr, esp_partition_mmap_handle_t *out_handle)
{
    assert(partition != NULL);
    if (offset > partition->size) {
        return ESP_ERR_INVALID_ARG;
    }
    if (size > partition->size - offset) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (partition->flash_chip != esp_flash_default_chip) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return spi_flash_mmap(partition->address + offset, size, (spi_flash_mmap_flag_t) flags, out_ptr, (spi_flash_mmap_handle_t *) out_handle);
}

void esp_partition_munmap(esp_partition_mmap_handle_t handle)
{
    spi_flash_munmap((spi_flash_mmap_handle_t) handle);
}

esp_err_t esp_partition_get_sha256(const esp_partition_t *partition, uint8_t *sha_256)
{
    return ESP_ERR_NOT_SUPPORTED;
}

bool esp_partition_check_identity(const esp_partition_t *partition_1, const esp_partition_t *partition_2)
{
    return false;
}

bool esp_partition_is_flash_region_writable(size_t addr, size_t size)
{
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
    for (; it != NULL; it = esp_partition_next(it)) {
        const esp_partition_t *p = esp_partition_get(it);
        if (p->readonly) {
            if (addr >= p->address && addr < p->address + p->size) {
                return false;
            }
            if (addr < p->address && addr + size > p->address) {
                return false;
            }
        }
    }
    return true;
}

bool esp_partition_main_flash_region_safe(size_t addr, size_t size)
{
    return addr > ESP_PARTITION_TABLE_OFFSET + ESP_PARTITION_TABLE_MAX_LEN;
}

uint32_t esp_partition_get_main_flash_sector_size(void)
{
    return SPI_FLASH_SEC_SIZE;
}
