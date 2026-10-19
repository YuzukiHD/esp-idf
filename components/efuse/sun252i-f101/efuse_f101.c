/*
 * SPDX-FileCopyrightText: 2026 Yuzuki Tsuru
 * SPDX-License-Identifier: Apache-2.0
 *
 * eFuse API on the fuses behind the SID controller: 512 bits in 16 words, read only.
 */
#include <string.h>
#include "esp_efuse.h"
#include "esp_efuse_table.h"
#include "esp_log.h"
#include "soc/sun252i_f101_ll.h"

static const char *TAG = "efuse";

#define WORDS   16
#define BITS    (WORDS * 32)

static const esp_efuse_desc_t s_chip_id[] = { { EFUSE_BLK0, 0, 128 } };
const esp_efuse_desc_t *ESP_EFUSE_CHIP_ID[] = { &s_chip_id[0], NULL };

static bool bit_of(unsigned pos)
{
    return (f101_sid_read_word(pos / 32) >> (pos % 32)) & 1;
}

int esp_efuse_get_field_size(const esp_efuse_desc_t *field[])
{
    int n = 0;

    for (; field && *field; field++) {
        n += (*field)->bit_count;
    }
    return n;
}

esp_err_t esp_efuse_read_field_blob(const esp_efuse_desc_t *field[], void *dst, size_t dst_size_bits)
{
    if (!field || !dst) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((size_t)esp_efuse_get_field_size(field) > dst_size_bits) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t *out = dst;
    size_t pos = 0;

    memset(out, 0, (dst_size_bits + 7) / 8);
    for (; *field; field++) {
        const esp_efuse_desc_t *f = *field;

        if (f->efuse_block != EFUSE_BLK0 || f->bit_start + f->bit_count > BITS) {
            return ESP_ERR_INVALID_ARG;
        }
        for (unsigned i = 0; i < f->bit_count; i++, pos++) {
            if (bit_of(f->bit_start + i)) {
                out[pos / 8] |= 1u << (pos % 8);
            }
        }
    }
    return ESP_OK;
}

bool esp_efuse_read_field_bit(const esp_efuse_desc_t *field[])
{
    uint8_t v = 0;

    return esp_efuse_read_field_blob(field, &v, 8) == ESP_OK && (v & 1);
}

esp_err_t esp_efuse_read_field_cnt(const esp_efuse_desc_t *field[], size_t *out_cnt)
{
    if (!field || !out_cnt) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t cnt = 0;

    for (; *field; field++) {
        for (unsigned i = 0; i < (*field)->bit_count; i++) {
            cnt += bit_of((*field)->bit_start + i);
        }
    }
    *out_cnt = cnt;
    return ESP_OK;
}

uint32_t esp_efuse_read_reg(esp_efuse_block_t blk, unsigned int num_reg)
{
    return (blk == EFUSE_BLK0 && num_reg < WORDS) ? f101_sid_read_word(num_reg) : 0;
}

esp_err_t esp_efuse_read_block(esp_efuse_block_t blk, void *dst, size_t offset_in_bits, size_t size_bits)
{
    if (blk != EFUSE_BLK0 || !dst || offset_in_bits + size_bits > BITS) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_efuse_desc_t d = { EFUSE_BLK0, offset_in_bits, size_bits };
    const esp_efuse_desc_t *f[] = { &d, NULL };

    return esp_efuse_read_field_blob(f, dst, size_bits);
}

/* the fuses cannot be written from here */
esp_err_t esp_efuse_write_field_blob(const esp_efuse_desc_t *field[], const void *src, size_t src_size_bits)
{
    ESP_LOGE(TAG, "the fuses are read only");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esp_efuse_write_field_cnt(const esp_efuse_desc_t *field[], size_t cnt)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esp_efuse_write_field_bit(const esp_efuse_desc_t *field[])
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esp_efuse_write_reg(esp_efuse_block_t blk, unsigned int num_reg, uint32_t val)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esp_efuse_write_block(esp_efuse_block_t blk, const void *src_key, size_t offset_in_bits, size_t size_bits)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esp_efuse_set_write_protect(esp_efuse_block_t blk)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esp_efuse_set_read_protect(esp_efuse_block_t blk)
{
    return ESP_ERR_NOT_SUPPORTED;
}

uint32_t esp_efuse_get_pkg_ver(void)
{
    return 0;
}
