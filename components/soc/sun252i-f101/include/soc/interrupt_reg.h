/*
 * SPDX-FileCopyrightText: 2026 Yuzuki Tsuru
 * SPDX-License-Identifier: Apache-2.0
 *
 * The C907 has the standard CLINT/PLIC, no interrupt matrix registers. The
 * names below keep the common riscv/esp_system sources compiling.
 */
#pragma once
#include "esp_bit_defs.h"   // BIT(), used by riscv/rv_utils.h

#include <stdint.h>

#define MTVEC_MODE_CSR          0
#define ETS_INT_WDT_INUM        24
#define ETS_CACHEERR_INUM       25
#define ETS_TG1_T0_INUM         10

static inline uint32_t rv_utils_intr_get_enabled_mask(void)
{
    uint32_t v;

    __asm volatile("csrr %0, mie" : "=r"(v));
    return v;
}

static inline void rv_utils_intr_edge_ack(unsigned int intr_num)
{
    (void)intr_num;
}
