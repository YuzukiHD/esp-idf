/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Register level access of the sun252i-f101 blocks the IDF port needs first:
 * DW-APB UART, PIO banks, CLINT timer, CCU bus gates.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define F101_REG32(a)          (*(volatile uint32_t *)(uintptr_t)(a))

#define F101_CCU_BASE          0x02001000u
#define F101_CCU_UART_BGR      (F101_CCU_BASE + 0x90Cu)
#define F101_PIO_BASE          0x02000000u
#define F101_PIO_BANK_STRIDE   0x30u
#define F101_UART_BASE(n)      (0x02500000u + (n) * 0x400u)
#define F101_CLINT_BASE        0x14000000u
#define F101_CLINT_MTIMECMP    (F101_CLINT_BASE + 0x4000u)
#define F101_CLINT_MTIME       (F101_CLINT_BASE + 0xBFF8u)
#define F101_MTIME_HZ          24000000u

/* ---- CLINT ------------------------------------------------------------ */

static inline uint64_t f101_mtime_get(void)
{
    uint32_t hi, lo, hi2;

    do {
        hi = F101_REG32(F101_CLINT_MTIME + 4);
        lo = F101_REG32(F101_CLINT_MTIME);
        hi2 = F101_REG32(F101_CLINT_MTIME + 4);
    } while (hi != hi2);
    return ((uint64_t)hi << 32) | lo;
}

static inline void f101_delay_us(uint32_t us)
{
    uint64_t end = f101_mtime_get() + (uint64_t)us * (F101_MTIME_HZ / 1000000u);

    while (f101_mtime_get() < end) {
    }
}

/* ---- UART (DW-APB, 32 bit register stride) ------------------------------ */

#define F101_UART_RBR_THR_DLL  0x00u
#define F101_UART_IER_DLH      0x04u
#define F101_UART_FCR_IIR      0x08u
#define F101_UART_LCR          0x0Cu
#define F101_UART_MCR          0x10u
#define F101_UART_LSR          0x14u
#define F101_UART_USR          0x7Cu

#define F101_UART_LSR_DR       0x01u
#define F101_UART_LSR_THRE     0x20u
#define F101_UART_LSR_TEMT     0x40u

/* Bus gate and reset of UARTn sit at bit n and 16 + n */
static inline void f101_uart_clock_enable(int n)
{
    F101_REG32(F101_CCU_UART_BGR) |= (1u << n) | (1u << (16 + n));
}

/*
 * 8N1, FIFO on. clk_hz is the UART module clock (the APB bus clock for this
 * block), the divisor is rounded to nearest.
 */
static inline void f101_uart_init(int n, uint32_t clk_hz, uint32_t baud)
{
    uint32_t b = F101_UART_BASE(n);
    uint32_t div = (clk_hz + 8u * baud) / (16u * baud);

    f101_uart_clock_enable(n);
    F101_REG32(b + F101_UART_IER_DLH) = 0;
    F101_REG32(b + F101_UART_FCR_IIR) = 0x07;   /* FIFO enable, reset both */
    F101_REG32(b + F101_UART_LCR) = 0x83;       /* 8N1, DLAB */
    F101_REG32(b + F101_UART_RBR_THR_DLL) = div & 0xff;
    F101_REG32(b + F101_UART_IER_DLH) = (div >> 8) & 0xff;
    F101_REG32(b + F101_UART_LCR) = 0x03;
}

static inline void f101_uart_putc(int n, char c)
{
    uint32_t b = F101_UART_BASE(n);

    while (!(F101_REG32(b + F101_UART_LSR) & F101_UART_LSR_THRE)) {
    }
    F101_REG32(b + F101_UART_RBR_THR_DLL) = (uint8_t)c;
}

static inline void f101_uart_flush(int n)
{
    while (!(F101_REG32(F101_UART_BASE(n) + F101_UART_LSR) & F101_UART_LSR_TEMT)) {
    }
}

static inline int f101_uart_getc(int n)
{
    uint32_t b = F101_UART_BASE(n);

    if (!(F101_REG32(b + F101_UART_LSR) & F101_UART_LSR_DR)) {
        return -1;
    }
    return F101_REG32(b + F101_UART_RBR_THR_DLL) & 0xff;
}

/* ---- PIO -------------------------------------------------------------- */

#define F101_PIN(bank, num)    ((bank) * 32 + (num))   /* PA = 0, PB = 1, ... */

enum { F101_GPIO_IN = 0, F101_GPIO_OUT = 1, F101_GPIO_DISABLE = 7 };
enum { F101_PULL_NONE = 0, F101_PULL_UP = 1, F101_PULL_DOWN = 2 };

static inline uint32_t f101_pio_bank(int pin)
{
    return F101_PIO_BASE + (uint32_t)(pin / 32) * F101_PIO_BANK_STRIDE;
}

/* function 0 = input, 1 = output, 2.. = peripheral mux */
static inline void f101_pin_mux(int pin, uint32_t mux)
{
    uint32_t r = f101_pio_bank(pin) + (uint32_t)((pin % 32) / 8) * 4;
    uint32_t sh = (uint32_t)(pin % 8) * 4;

    F101_REG32(r) = (F101_REG32(r) & ~(0xfu << sh)) | (mux << sh);
}

static inline void f101_pin_pull(int pin, uint32_t pull)
{
    uint32_t r = f101_pio_bank(pin) + 0x24u + (uint32_t)((pin % 32) / 16) * 4;
    uint32_t sh = (uint32_t)(pin % 16) * 2;

    F101_REG32(r) = (F101_REG32(r) & ~(0x3u << sh)) | (pull << sh);
}

static inline void f101_pin_drive(int pin, uint32_t level)
{
    uint32_t r = f101_pio_bank(pin) + 0x14u + (uint32_t)((pin % 32) / 16) * 4;
    uint32_t sh = (uint32_t)(pin % 16) * 2;

    F101_REG32(r) = (F101_REG32(r) & ~(0x3u << sh)) | ((level & 3u) << sh);
}

static inline void f101_gpio_set_level(int pin, int level)
{
    uint32_t r = f101_pio_bank(pin) + 0x10u;

    if (level) {
        F101_REG32(r) |= 1u << (pin % 32);
    } else {
        F101_REG32(r) &= ~(1u << (pin % 32));
    }
}

static inline int f101_gpio_get_level(int pin)
{
    return (F101_REG32(f101_pio_bank(pin) + 0x10u) >> (pin % 32)) & 1;
}


/* ---- SID (read only fuses) ---------------------------------------------- */
#define F101_SID_BASE          0x03006000u

/* One 32 bit fuse word (index 0..15): word 0..3 hold the 128 bit chip id */
static inline uint32_t f101_sid_read_word(unsigned int word)
{
    uint32_t ctl;
    int tries = 100000;

    F101_REG32(F101_SID_BASE + 0x04u) = (F101_REG32(F101_SID_BASE + 0x04u) & ~0xfu) | (word & 0xfu);
    ctl = F101_REG32(F101_SID_BASE + 0x00u) & 0x0000fffcu;
    F101_REG32(F101_SID_BASE + 0x00u) = ctl | (0xADBFu << 16) | 2u;     /* read command with its key */
    while ((F101_REG32(F101_SID_BASE + 0x00u) & 2u) && --tries) {
    }
    F101_REG32(F101_SID_BASE + 0x00u) = ctl;
    return F101_REG32(F101_SID_BASE + 0x0cu);
}

#ifdef __cplusplus
}
#endif
