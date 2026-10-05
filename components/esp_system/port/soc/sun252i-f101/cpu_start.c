/*
 * SPDX-FileCopyrightText: 2026 Yuzuki Tsuru
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <stdbool.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_attr.h"
#include "esp_private/startup_internal.h"
#include "soc/sun252i_f101_ll.h"

ESP_LOG_ATTR_TAG(TAG, "cpu_start");

extern void start_cpu0(void) __attribute__((noreturn));

extern int _thread_local_data_start, _thread_local_data_end;
extern int _thread_local_bss_start, _thread_local_bss_end;

/* thread local storage of the code that runs before the scheduler */
static uint8_t s_boot_tls[128] __attribute__((aligned(16)));

static void boot_tls_init(void)
{
    uint32_t tdata = (uint32_t)&_thread_local_data_end - (uint32_t)&_thread_local_data_start;
    uint32_t tbss = (uint32_t)&_thread_local_bss_end - (uint32_t)&_thread_local_bss_start;

    if (tdata + tbss > sizeof(s_boot_tls)) {
        for (;;) {
        }
    }
    __builtin_memcpy(s_boot_tls, &_thread_local_data_start, tdata);
    __builtin_memset(s_boot_tls + tdata, 0, tbss);
    __asm volatile("mv tp, %0" :: "r"(s_boot_tls));
}

void __attribute__((noreturn)) call_start_cpu0_c(void)
{
    boot_tls_init();
    f101_uart_init(CONFIG_ESP_CONSOLE_ROM_SERIAL_PORT_NUM, 24000000, CONFIG_ESP_CONSOLE_UART_BAUDRATE);

    esp_rom_install_uart_printf();
    esp_rom_printf("\nESP-ROM:sun252i-f101-fel\n");
    esp_rom_printf("Build:" __DATE__ "\n");
    esp_rom_printf("rst:0x1 (POWERON),boot:0x0 (FEL_DOWNLOAD)\n");


    ESP_EARLY_LOGI(TAG, "Pro cpu up.");

    start_cpu0();
}
