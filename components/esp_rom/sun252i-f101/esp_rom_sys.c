/*
 * SPDX-FileCopyrightText: 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include "sdkconfig.h"
#include "soc/sun252i_f101_ll.h"
#include "esp_rom_sys.h"
#include "esp_rom_serial_output.h"

/* The boot helper brings up the console UART; the port makes it 115200 8N1 */
#define ROM_CONSOLE_UART   CONFIG_ESP_CONSOLE_ROM_SERIAL_PORT_NUM

int esp_rom_output_tx_one_char(uint8_t c)
{
    f101_uart_putc(ROM_CONSOLE_UART, (char)c);
    return 0;
}

void esp_rom_output_putc(char c)
{
    if (c == '\n') {
        f101_uart_putc(ROM_CONSOLE_UART, '\r');
    }
    f101_uart_putc(ROM_CONSOLE_UART, c);
}

int esp_rom_output_rx_one_char(uint8_t *c)
{
    int v = f101_uart_getc(ROM_CONSOLE_UART);

    if (v < 0) {
        return -1;
    }
    *c = (uint8_t)v;
    return 0;
}

void esp_rom_output_tx_wait_idle(uint8_t serial_num)
{
    f101_uart_flush(serial_num);
}

void esp_rom_output_flush_tx(uint8_t serial_num)
{
    f101_uart_flush(serial_num);
}

void esp_rom_delay_us(uint32_t us)
{
    f101_delay_us(us);
}

soc_reset_reason_t esp_rom_get_reset_reason(int cpu_no)
{
    return RESET_REASON_CHIP_POWER_ON;
}

void esp_rom_software_reset_system(void)
{
    /* watchdog in reset mode, shortest timeout */
    F101_REG32(0x06011000u + 0x18) = 0x16aa0000u;
    F101_REG32(0x06011000u + 0x00) = 0;
    F101_REG32(0x06011000u + 0x04) = 1;
    F101_REG32(0x06011000u + 0x1c) = 0x3f;
    F101_REG32(0x06011000u + 0x14) = 0x16aa0000u | 1u;
    F101_REG32(0x06011000u + 0x18) = 0x16aa0000u | (1u << 4) | 1u;
    F101_REG32(0x06011000u + 0x10) = (0x0a57u << 1) | 1u;
    for (;;) {
    }
}

void esp_rom_uart_set_as_console(uint8_t uart_no)
{
    (void)uart_no;
}

void ets_install_putc1(void (*p)(char c))
{
    (void)p;
}

void ets_install_putc2(void (*p)(char c))
{
    (void)p;
}

uint32_t esp_rom_get_cpu_ticks_per_us(void)
{
    return CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
}
