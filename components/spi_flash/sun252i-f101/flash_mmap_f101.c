/*
 * SPDX-FileCopyrightText: 2026 Yuzuki Tsuru
 * SPDX-License-Identifier: Apache-2.0
 *
 * The flash cannot be mapped into the address space of this SoC, so the
 * spi_flash_mmap() API returns a RAM copy of the requested region: the data
 * is read through esp_flash, a write to the flash afterwards is not reflected
 * in a copy that is still mapped.
 */

#include <stdlib.h>
#include "esp_heap_caps.h"
#include "esp_flash.h"
#include "spi_flash_mmap.h"

typedef struct {
    void *data;
} mapping_t;

esp_err_t spi_flash_mmap(size_t src_addr, size_t size, spi_flash_mmap_flag_t flags,
                         const void **out_ptr, spi_flash_mmap_handle_t *out_handle)
{
    if (!out_ptr || !out_handle || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    mapping_t *m = malloc(sizeof(*m));
    if (!m) {
        return ESP_ERR_NO_MEM;
    }
    m->data = heap_caps_aligned_alloc(64, (size + 63u) & ~63u, MALLOC_CAP_DEFAULT);
    if (!m->data) {
        free(m);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_flash_read(esp_flash_default_chip, m->data, src_addr, size);
    if (err != ESP_OK) {
        free(m->data);
        free(m);
        return err;
    }
    *out_ptr = m->data;
    *out_handle = (spi_flash_mmap_handle_t)(uintptr_t)m;
    return ESP_OK;
}

esp_err_t spi_flash_mmap_pages(const int *pages, size_t page_count, spi_flash_mmap_flag_t flags,
                               const void **out_ptr, spi_flash_mmap_handle_t *out_handle)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void spi_flash_munmap(spi_flash_mmap_handle_t handle)
{
    mapping_t *m = (mapping_t *)(uintptr_t)handle;

    if (m) {
        free(m->data);
        free(m);
    }
}

void spi_flash_mmap_dump(void)
{
}

uint32_t spi_flash_mmap_get_free_pages(spi_flash_mmap_memory_t memory)
{
    return 0x100;
}

size_t spi_flash_cache2phys(const void *cached)
{
    return SPI_FLASH_CACHE2PHYS_FAIL;
}

const void *spi_flash_phys2cache(size_t phys_offs, spi_flash_mmap_memory_t memory)
{
    return NULL;
}
