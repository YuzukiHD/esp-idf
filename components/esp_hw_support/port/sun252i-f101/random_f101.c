/*
 * SPDX-FileCopyrightText: 2026 Yuzuki Tsuru
 * SPDX-License-Identifier: Apache-2.0
 *
 * Not a cryptographic source: xorshift seeded from the timer, the chip id
 * and previous output.
 */

#include <stdint.h>
#include <string.h>
#include "esp_random.h"
#include "soc/sun252i_f101_ll.h"

static uint32_t s_state;

#define sid_word f101_sid_read_word

uint32_t esp_random(void)
{
    if (s_state == 0) {
        s_state = sid_word(0) ^ sid_word(1) ^ sid_word(2) ^ sid_word(3) ^ (uint32_t)f101_mtime_get();
        if (s_state == 0) {
            s_state = 0x2545f491u;
        }
    }
    s_state ^= (uint32_t)f101_mtime_get();
    s_state ^= s_state << 13;
    s_state ^= s_state >> 17;
    s_state ^= s_state << 5;
    return s_state;
}

void esp_fill_random(void *buf, size_t len)
{
    uint8_t *p = buf;

    while (len) {
        uint32_t r = esp_random();
        size_t n = len < 4 ? len : 4;

        memcpy(p, &r, n);
        p += n;
        len -= n;
    }
}
