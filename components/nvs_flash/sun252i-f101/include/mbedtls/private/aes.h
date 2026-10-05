/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* NVS encryption is not available on this target: the XTS entry points of nvs_xts_stub_f101.c refuse every call */
typedef struct {
    int unused;
} mbedtls_aes_xts_context;

#define MBEDTLS_AES_ENCRYPT 1
#define MBEDTLS_AES_DECRYPT 0

void mbedtls_aes_xts_init(mbedtls_aes_xts_context *ctx);
void mbedtls_aes_xts_free(mbedtls_aes_xts_context *ctx);
int mbedtls_aes_xts_setkey_enc(mbedtls_aes_xts_context *ctx, const unsigned char *key, unsigned int keybits);
int mbedtls_aes_xts_setkey_dec(mbedtls_aes_xts_context *ctx, const unsigned char *key, unsigned int keybits);
int mbedtls_aes_crypt_xts(mbedtls_aes_xts_context *ctx, int mode, size_t length, const unsigned char data_unit[16],
                          const unsigned char *input, unsigned char *output);
