/*
 * SPDX-FileCopyrightText: 2026 Yuzuki Tsuru
 * SPDX-License-Identifier: Apache-2.0
 */
#include "mbedtls/private/aes.h"

#define NOT_SUPPORTED   (-0x6e)

void mbedtls_aes_xts_init(mbedtls_aes_xts_context *ctx)
{
}

void mbedtls_aes_xts_free(mbedtls_aes_xts_context *ctx)
{
}

int mbedtls_aes_xts_setkey_enc(mbedtls_aes_xts_context *ctx, const unsigned char *key, unsigned int keybits)
{
    return NOT_SUPPORTED;
}

int mbedtls_aes_xts_setkey_dec(mbedtls_aes_xts_context *ctx, const unsigned char *key, unsigned int keybits)
{
    return NOT_SUPPORTED;
}

int mbedtls_aes_crypt_xts(mbedtls_aes_xts_context *ctx, int mode, size_t length, const unsigned char data_unit[16],
                          const unsigned char *input, unsigned char *output)
{
    return NOT_SUPPORTED;
}
