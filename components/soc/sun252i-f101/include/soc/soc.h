/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

#pragma once

/* the whole PSRAM is one unified instruction/data memory, there is no ROM or flash window */
#define SOC_IRAM_LOW                0x40010000UL
#define SOC_IRAM_HIGH               0x41000000UL
#define SOC_DRAM_LOW                0x40010000UL
#define SOC_DRAM_HIGH               0x41000000UL
#define SOC_DIRAM_IRAM_LOW          0x40010000UL
#define SOC_DIRAM_IRAM_HIGH         0x41000000UL
#define SOC_DIRAM_DRAM_LOW          0x40010000UL
#define SOC_DIRAM_DRAM_HIGH         0x41000000UL
#define SOC_IROM_LOW                0x40010000UL
#define SOC_IROM_HIGH               0x40010000UL
#define SOC_DROM_LOW                0x40010000UL
#define SOC_DROM_HIGH               0x40010000UL
#define SOC_IROM_MASK_LOW           0x100UL
#define SOC_IROM_MASK_HIGH          0x100UL
#define SOC_DROM_MASK_LOW           0x200UL
#define SOC_DROM_MASK_HIGH          0x200UL
#define SOC_MAX_CONTIGUOUS_RAM_SIZE (SOC_IRAM_HIGH - SOC_IRAM_LOW)
#define SOC_MEM_INTERNAL_LOW        0x40010000UL
#define SOC_MEM_INTERNAL_HIGH       0x41000000UL
#define SOC_MEM_INTERNAL_LOW1       0x40010000UL
#define SOC_MEM_INTERNAL_HIGH1      0x41000000UL
#define SOC_DMA_LOW                 0x40010000UL
#define SOC_DMA_HIGH                0x41000000UL
#define SOC_RTC_IRAM_LOW            0x0UL
#define SOC_RTC_IRAM_HIGH           0x0UL
#define SOC_RTC_DRAM_LOW            0x0UL
#define SOC_RTC_DRAM_HIGH           0x0UL
#define SOC_RTC_DATA_LOW            0x0UL
#define SOC_RTC_DATA_HIGH           0x0UL
#define SOC_EXTRAM_LOW              0x0UL
#define SOC_EXTRAM_HIGH             0x0UL
#define SOC_DIRAM_ROM_RESERVE_HIGH  0x41000000UL
#define SOC_BYTE_ACCESSIBLE_LOW     0x40010000UL
#define SOC_BYTE_ACCESSIBLE_HIGH    0x41000000UL
#define SOC_PERIPHERAL_LOW          0x02000000UL
#define SOC_PERIPHERAL_HIGH         0x14010000UL
#define SOC_ROM_STACK_START         0x41000000UL
#define SOC_ROM_STACK_SIZE          0x0

#ifndef __ASSEMBLER__

//write value to register
#define REG_WRITE(_r, _v)  do {                                                                                        \
            (*(volatile uint32_t *)(_r)) = (_v);                                                                       \
        } while(0)

//read value from register
#define REG_READ(_r) ({                                                                                                \
            ((uint32_t)*(volatile uintptr_t *)(_r));                                                                              \
        })

#endif

#define SOC_RAM_LOW                 0x40010000UL
#define SOC_RAM_HIGH                0x41000000UL
