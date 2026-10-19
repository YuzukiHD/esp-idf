/*
 * SPDX-FileCopyrightText: 2026 Yuzuki Tsuru
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include "sdkconfig.h"
#include "esp_private/esp_clk.h"
#include "esp_private/startup_internal.h"
#include "esp_log.h"
#include "soc/sun252i_f101_ll.h"

/* PLL_CPU: N multiplies the 24 MHz crystal, the dividers P, M0, M1 divide the result */
#define PLL_CPU_REG         (F101_CCU_BASE + 0x0000u)
#define PLL_CPU_SSC_REG     (F101_CCU_BASE + 0x0200u)
#define PLL_CPU_M1_MASK     (0xfu << 0)
#define PLL_CPU_N_MASK      (0xffu << 8)
#define PLL_CPU_P_MASK      (0x7u << 16)
#define PLL_CPU_M0_MASK     (0x3u << 20)
#define PLL_CPU_UPDATE      (1u << 26)
#define PLL_CPU_SSC_MODE    (1u << 31)
#define PLL_CPU_MAX_HZ      1008000000u
#define PLL_CPU_N_MIN       12u
#define PLL_CPU_N_MAX       125u
#define CPU_STEP_HZ         72000000u

static const char *TAG = "clk";

static uint32_t pll_cpu_div(uint32_t reg)
{
    return (((reg & PLL_CPU_P_MASK) >> 16) + 1) * (((reg & PLL_CPU_M0_MASK) >> 20) + 1) * ((reg & PLL_CPU_M1_MASK) + 1);
}

static uint32_t pll_cpu_hz(void)
{
    uint32_t reg = F101_REG32(PLL_CPU_REG);

    return 24000000u / pll_cpu_div(reg) * ((reg & PLL_CPU_N_MASK) >> 8);
}

/*
 * The factor N is latched by the update bit while the spread spectrum mode is on; the mode is then
 * switched off and latched again. The CPU keeps running from the PLL throughout.
 */
static void pll_cpu_set_n(uint32_t n)
{
    F101_REG32(PLL_CPU_SSC_REG) |= PLL_CPU_SSC_MODE;
    F101_REG32(PLL_CPU_REG) = (F101_REG32(PLL_CPU_REG) & ~PLL_CPU_N_MASK) | (n << 8) | PLL_CPU_UPDATE;
    while (F101_REG32(PLL_CPU_REG) & PLL_CPU_UPDATE) {
    }
    F101_REG32(PLL_CPU_SSC_REG) &= ~PLL_CPU_SSC_MODE;
    F101_REG32(PLL_CPU_REG) |= PLL_CPU_UPDATE;
    while (F101_REG32(PLL_CPU_REG) & PLL_CPU_UPDATE) {
    }
    f101_delay_us(200);
}

/* write and read back patterns through the memory: a CPU that is too fast for the supply fails here */
static bool memory_self_test(void)
{
    static volatile uint32_t buf[1024];

    for (uint32_t pass = 0; pass < 4; pass++) {
        uint32_t seed = 0x9e3779b9u * (pass + 1);

        for (uint32_t i = 0; i < 1024; i++) {
            buf[i] = seed ^ (i * 0x01000193u);
        }
        for (uint32_t i = 0; i < 1024; i++) {
            if (buf[i] != (seed ^ (i * 0x01000193u))) {
                return false;
            }
        }
    }
    return true;
}

/* raise the CPU clock in steps of 72 MHz and test the memory after each one; a failing step is undone */
static void cpu_freq_set(uint32_t target_hz)
{
    uint32_t div = pll_cpu_div(F101_REG32(PLL_CPU_REG));
    uint32_t unit = 24000000u / div;
    uint32_t hz = pll_cpu_hz();

    if (target_hz > PLL_CPU_MAX_HZ || target_hz / unit < PLL_CPU_N_MIN || target_hz / unit > PLL_CPU_N_MAX) {
        ESP_EARLY_LOGW(TAG, "CPU %u Hz is out of range, staying at %u Hz", (unsigned)target_hz, (unsigned)hz);
        return;
    }
    while (hz != target_hz) {
        uint32_t next = hz < target_hz ? (target_hz - hz > CPU_STEP_HZ ? hz + CPU_STEP_HZ : target_hz)
                                       : (hz - target_hz > CPU_STEP_HZ ? hz - CPU_STEP_HZ : target_hz);

        pll_cpu_set_n((next + unit / 2) / unit);
        if (!memory_self_test()) {
            ESP_EARLY_LOGE(TAG, "memory test failed at %u Hz, back to %u Hz", (unsigned)next, (unsigned)hz);
            pll_cpu_set_n(hz / unit);
            return;
        }
        hz = pll_cpu_hz();
    }
}

ESP_SYSTEM_INIT_FN(init_cpu_freq, CORE, BIT(0), 60)
{
    cpu_freq_set(CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1000000u);
    return 0;
}

int esp_clk_cpu_freq(void)
{
    return (int)pll_cpu_hz();
}

int esp_clk_apb_freq(void)
{
    return 100000000;
}

int esp_clk_xtal_freq(void)
{
    return 24000000;
}

uint32_t esp_clk_slowclk_cal_get(void)
{
    return 0;
}

void esp_clk_slowclk_cal_set(uint32_t value)
{
    (void)value;
}

/* microseconds since power up */
uint64_t esp_clk_rtc_time(void)
{
    return f101_mtime_get() / (F101_MTIME_HZ / 1000000u);
}

void esp_clk_private_lock(void)
{
}

void esp_clk_private_unlock(void)
{
}

#include "esp_rtc_time.h"

uint64_t esp_rtc_get_time_us(void)
{
    return esp_clk_rtc_time();
}
