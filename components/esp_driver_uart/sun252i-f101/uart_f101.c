/*
 * SPDX-FileCopyrightText: 2026 Yuzuki Tsuru
 * SPDX-License-Identifier: Apache-2.0
 *
 * driver/uart.h on the DW-APB UARTs. Receive is interrupt driven into a
 * stream buffer, transmit is polled into the FIFO.
 */

#include <string.h>
#include "sdkconfig.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_intr_alloc.h"
#include "driver/uart.h"
#include "soc/sun252i_f101_ll.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/queue.h"

#define UART_CLK_HZ     24000000u
#define NUM_UARTS       SOC_UART_HP_NUM
#define PLIC_SRC(n)     (2 + (n))

#define LCR_PEN         (1u << 3)
#define LCR_EPS         (1u << 4)
#define LCR_STOP        (1u << 2)
#define IER_ERBFI       (1u << 0)
#define IER_ELSI        (1u << 2)
#define MCR_LOOP        (1u << 4)
#define LSR_OE          (1u << 1)
#define LSR_PE          (1u << 2)
#define LSR_FE          (1u << 3)
#define LSR_BI          (1u << 4)

static const char *TAG = "uart";

typedef struct {
    bool installed;
    uint32_t baud;
    StreamBufferHandle_t rx;
    QueueHandle_t events;
    intr_handle_t intr;
    int rx_size;
} uart_obj_t;

static uart_obj_t s_uart[NUM_UARTS];

#define CHECK_PORT(n) ESP_RETURN_ON_FALSE((n) >= 0 && (n) < NUM_UARTS, ESP_ERR_INVALID_ARG, TAG, "invalid uart port")
#define CHECK_INST(n) do { CHECK_PORT(n); ESP_RETURN_ON_FALSE(s_uart[n].installed, ESP_FAIL, TAG, "driver not installed"); } while (0)

static inline uint32_t base(int n)
{
    return F101_UART_BASE(n);
}

static void uart_isr(void *arg)
{
    int n = (int)(intptr_t)arg;
    uart_obj_t *u = &s_uart[n];
    uint32_t b = base(n);
    BaseType_t woken = pdFALSE;
    uint32_t iir;

    while (((iir = F101_REG32(b + F101_UART_FCR_IIR)) & 0xf) != 1) {
        uint32_t id = iir & 0xf;

        if (id == 0x4 || id == 0xc) {                 /* received data / character timeout */
            uint8_t tmp[32];
            int cnt = 0;

            while ((F101_REG32(b + F101_UART_LSR) & F101_UART_LSR_DR) && cnt < (int)sizeof(tmp)) {
                tmp[cnt++] = (uint8_t)F101_REG32(b + F101_UART_RBR_THR_DLL);
            }
            size_t sent = xStreamBufferSendFromISR(u->rx, tmp, cnt, &woken);
            if (u->events && sent) {
                uart_event_t ev = { .type = UART_DATA, .size = sent };
                xQueueSendFromISR(u->events, &ev, &woken);
            }
            if (sent < (size_t)cnt && u->events) {
                uart_event_t ev = { .type = UART_BUFFER_FULL };
                xQueueSendFromISR(u->events, &ev, &woken);
            }
        } else if (id == 0x6) {                       /* line status */
            uint32_t lsr = F101_REG32(b + F101_UART_LSR);
            if (u->events) {
                uart_event_t ev = { .type = (lsr & LSR_OE) ? UART_FIFO_OVF :
                                            (lsr & LSR_PE) ? UART_PARITY_ERR :
                                            (lsr & LSR_FE) ? UART_FRAME_ERR : UART_BREAK };
                xQueueSendFromISR(u->events, &ev, &woken);
            }
        } else if (id == 0x7) {                       /* busy detect: reading USR clears it */
            (void)F101_REG32(b + F101_UART_USR);
        } else {
            (void)F101_REG32(b + F101_UART_RBR_THR_DLL);
        }
    }
    portYIELD_FROM_ISR(woken);
}

static void set_baud_regs(int n, uint32_t baud)
{
    uint32_t b = base(n);
    uint32_t div = (UART_CLK_HZ + 8u * baud) / (16u * baud);
    uint32_t lcr = F101_REG32(b + F101_UART_LCR);

    if (div == 0) {
        div = 1;
    }
    F101_REG32(b + F101_UART_LCR) = lcr | 0x80;
    F101_REG32(b + F101_UART_RBR_THR_DLL) = div & 0xff;
    F101_REG32(b + F101_UART_IER_DLH) = (div >> 8) & 0xff;
    F101_REG32(b + F101_UART_LCR) = lcr;
    s_uart[n].baud = UART_CLK_HZ / (16u * div);
}

esp_err_t uart_driver_install(uart_port_t uart_num, int rx_buffer_size, int tx_buffer_size, int queue_size,
                              QueueHandle_t *uart_queue, int intr_alloc_flags)
{
    (void)tx_buffer_size;
    (void)intr_alloc_flags;
    CHECK_PORT(uart_num);
    ESP_RETURN_ON_FALSE(rx_buffer_size > 0, ESP_ERR_INVALID_ARG, TAG, "rx buffer size must be > 0");
    ESP_RETURN_ON_FALSE(!s_uart[uart_num].installed, ESP_FAIL, TAG, "driver already installed");

    uart_obj_t *u = &s_uart[uart_num];

    memset(u, 0, sizeof(*u));
    u->rx = xStreamBufferCreate(rx_buffer_size, 1);
    ESP_RETURN_ON_FALSE(u->rx, ESP_ERR_NO_MEM, TAG, "no memory");
    if (queue_size > 0 && uart_queue) {
        u->events = xQueueCreate(queue_size, sizeof(uart_event_t));
        *uart_queue = u->events;
    }
    f101_uart_clock_enable(uart_num);
    F101_REG32(base(uart_num) + F101_UART_FCR_IIR) = 0x07 | (2u << 6);   /* FIFO on, reset, RX trigger 1/2 */
    ESP_RETURN_ON_ERROR(esp_intr_alloc(PLIC_SRC(uart_num), ESP_INTR_FLAG_LEVEL1, uart_isr,
                                       (void *)(intptr_t)uart_num, &u->intr), TAG, "intr alloc failed");
    u->installed = true;
    set_baud_regs(uart_num, u->baud ? u->baud : 115200);
    F101_REG32(base(uart_num) + F101_UART_IER_DLH) = IER_ERBFI | IER_ELSI;
    return ESP_OK;
}

esp_err_t uart_driver_delete(uart_port_t uart_num)
{
    CHECK_INST(uart_num);
    uart_obj_t *u = &s_uart[uart_num];

    F101_REG32(base(uart_num) + F101_UART_IER_DLH) = 0;
    esp_intr_free(u->intr);
    vStreamBufferDelete(u->rx);
    if (u->events) {
        vQueueDelete(u->events);
    }
    memset(u, 0, sizeof(*u));
    return ESP_OK;
}

bool uart_is_driver_installed(uart_port_t uart_num)
{
    return uart_num >= 0 && uart_num < NUM_UARTS && s_uart[uart_num].installed;
}

esp_err_t uart_param_config(uart_port_t uart_num, const uart_config_t *cfg)
{
    CHECK_PORT(uart_num);
    ESP_RETURN_ON_FALSE(cfg, ESP_ERR_INVALID_ARG, TAG, "null config");
    uint32_t b = base(uart_num);
    uint32_t lcr = (uint32_t)cfg->data_bits & 3;

    f101_uart_clock_enable(uart_num);
    if (cfg->stop_bits != UART_STOP_BITS_1) {
        lcr |= LCR_STOP;
    }
    if (cfg->parity != UART_PARITY_DISABLE) {
        lcr |= LCR_PEN;
        if (cfg->parity == UART_PARITY_EVEN) {
            lcr |= LCR_EPS;
        }
    }
    F101_REG32(b + F101_UART_LCR) = lcr;
    set_baud_regs(uart_num, cfg->baud_rate);
    return ESP_OK;
}

/* pins that can carry the TX / RX signal of each UART and the mux value that selects it */
typedef struct {
    uint8_t pin;        /* bank * 32 + number */
    uint8_t mux;
} uart_pin_opt_t;

#define PIN(bank, n)    ((uint8_t)((bank) * 32 + (n)))
enum { PB = 1, PE = 4, PF = 5 };

static const struct {
    uart_pin_opt_t tx[2];
    uart_pin_opt_t rx[2];
} s_uart_pins[NUM_UARTS] = {
    [0] = { { { PIN(PF, 2), 3 } },                         { { PIN(PF, 4), 3 } } },
    [1] = { { { PIN(PF, 0), 4 }, { PIN(PB, 0), 4 } },      { { PIN(PF, 1), 4 }, { PIN(PB, 1), 4 } } },
    [2] = { { { PIN(PF, 4), 6 } },                         { { PIN(PF, 5), 6 } } },
    [3] = { { { PIN(PE, 8), 6 } },                         { { PIN(PE, 9), 6 } } },
    [4] = { { { PIN(PE, 2), 6 } },                         { { PIN(PE, 3), 6 } } },
    [5] = { { { PIN(PE, 4), 7 } },                         { { PIN(PE, 5), 7 } } },
};

static int find_mux(const uart_pin_opt_t *opts, int pin)
{
    for (int i = 0; i < 2; i++) {
        if (opts[i].pin == pin && (opts[i].pin || opts[i].mux)) {
            return opts[i].mux;
        }
    }
    return -1;
}

esp_err_t _uart_set_pin6(uart_port_t uart_num, int tx_io_num, int rx_io_num, int rts_io_num, int cts_io_num,
                        int dtr_io_num, int dsr_io_num)
{
    CHECK_PORT(uart_num);
    ESP_RETURN_ON_FALSE(rts_io_num < 0 && cts_io_num < 0 && dtr_io_num < 0 && dsr_io_num < 0,
                        ESP_ERR_NOT_SUPPORTED, TAG, "no flow control pins on this target");
    int tx_mux = -1, rx_mux = -1;

    if (tx_io_num >= 0) {
        tx_mux = find_mux(s_uart_pins[uart_num].tx, tx_io_num);
        ESP_RETURN_ON_FALSE(tx_mux >= 0, ESP_ERR_INVALID_ARG, TAG, "gpio %d cannot be the TX of uart %d", tx_io_num, uart_num);
    }
    if (rx_io_num >= 0) {
        rx_mux = find_mux(s_uart_pins[uart_num].rx, rx_io_num);
        ESP_RETURN_ON_FALSE(rx_mux >= 0, ESP_ERR_INVALID_ARG, TAG, "gpio %d cannot be the RX of uart %d", rx_io_num, uart_num);
    }
    if (tx_mux >= 0) {
        f101_pin_mux(tx_io_num, tx_mux);
    }
    if (rx_mux >= 0) {
        f101_pin_mux(rx_io_num, rx_mux);
        f101_pin_pull(rx_io_num, F101_PULL_UP);
    }
    return ESP_OK;
}

esp_err_t uart_set_baudrate(uart_port_t uart_num, uint32_t baudrate)
{
    CHECK_PORT(uart_num);
    ESP_RETURN_ON_FALSE(baudrate > 0, ESP_ERR_INVALID_ARG, TAG, "invalid baud rate");
    set_baud_regs(uart_num, baudrate);
    return ESP_OK;
}

esp_err_t uart_get_baudrate(uart_port_t uart_num, uint32_t *baudrate)
{
    CHECK_PORT(uart_num);
    ESP_RETURN_ON_FALSE(baudrate, ESP_ERR_INVALID_ARG, TAG, "null pointer");
    *baudrate = s_uart[uart_num].baud;
    return ESP_OK;
}

esp_err_t uart_get_sclk_freq(uart_sclk_t sclk, uint32_t *out_freq_hz)
{
    (void)sclk;
    ESP_RETURN_ON_FALSE(out_freq_hz, ESP_ERR_INVALID_ARG, TAG, "null pointer");
    *out_freq_hz = UART_CLK_HZ;
    return ESP_OK;
}

esp_err_t uart_set_word_length(uart_port_t uart_num, uart_word_length_t data_bit)
{
    CHECK_PORT(uart_num);
    uint32_t r = base(uart_num) + F101_UART_LCR;

    F101_REG32(r) = (F101_REG32(r) & ~3u) | ((uint32_t)data_bit & 3);
    return ESP_OK;
}

esp_err_t uart_get_word_length(uart_port_t uart_num, uart_word_length_t *data_bit)
{
    CHECK_PORT(uart_num);
    *data_bit = (uart_word_length_t)(F101_REG32(base(uart_num) + F101_UART_LCR) & 3);
    return ESP_OK;
}

esp_err_t uart_set_stop_bits(uart_port_t uart_num, uart_stop_bits_t stop_bits)
{
    CHECK_PORT(uart_num);
    uint32_t r = base(uart_num) + F101_UART_LCR;

    F101_REG32(r) = (F101_REG32(r) & ~LCR_STOP) | (stop_bits != UART_STOP_BITS_1 ? LCR_STOP : 0);
    return ESP_OK;
}

esp_err_t uart_get_stop_bits(uart_port_t uart_num, uart_stop_bits_t *stop_bits)
{
    CHECK_PORT(uart_num);
    *stop_bits = (F101_REG32(base(uart_num) + F101_UART_LCR) & LCR_STOP) ? UART_STOP_BITS_2 : UART_STOP_BITS_1;
    return ESP_OK;
}

esp_err_t uart_set_parity(uart_port_t uart_num, uart_parity_t parity_mode)
{
    CHECK_PORT(uart_num);
    uint32_t r = base(uart_num) + F101_UART_LCR;
    uint32_t v = F101_REG32(r) & ~(LCR_PEN | LCR_EPS);

    if (parity_mode != UART_PARITY_DISABLE) {
        v |= LCR_PEN | (parity_mode == UART_PARITY_EVEN ? LCR_EPS : 0);
    }
    F101_REG32(r) = v;
    return ESP_OK;
}

esp_err_t uart_get_parity(uart_port_t uart_num, uart_parity_t *parity_mode)
{
    CHECK_PORT(uart_num);
    uint32_t v = F101_REG32(base(uart_num) + F101_UART_LCR);

    *parity_mode = !(v & LCR_PEN) ? UART_PARITY_DISABLE : ((v & LCR_EPS) ? UART_PARITY_EVEN : UART_PARITY_ODD);
    return ESP_OK;
}

esp_err_t uart_set_loop_back(uart_port_t uart_num, bool loop_back_en)
{
    CHECK_PORT(uart_num);
    uint32_t r = base(uart_num) + F101_UART_MCR;

    F101_REG32(r) = (F101_REG32(r) & ~MCR_LOOP) | (loop_back_en ? MCR_LOOP : 0);
    return ESP_OK;
}

esp_err_t uart_enable_rx_intr(uart_port_t uart_num)
{
    CHECK_PORT(uart_num);
    F101_REG32(base(uart_num) + F101_UART_IER_DLH) |= IER_ERBFI;
    return ESP_OK;
}

esp_err_t uart_disable_rx_intr(uart_port_t uart_num)
{
    CHECK_PORT(uart_num);
    F101_REG32(base(uart_num) + F101_UART_IER_DLH) &= ~IER_ERBFI;
    return ESP_OK;
}

esp_err_t uart_set_hw_flow_ctrl(uart_port_t uart_num, uart_hw_flowcontrol_t flow_ctrl, uint8_t rx_thresh)
{
    CHECK_PORT(uart_num);
    (void)rx_thresh;
    return flow_ctrl == UART_HW_FLOWCTRL_DISABLE ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
}

esp_err_t uart_wait_tx_done(uart_port_t uart_num, uint32_t ticks_to_wait)
{
    CHECK_PORT(uart_num);
    TickType_t end = xTaskGetTickCount() + ticks_to_wait;

    while (!(F101_REG32(base(uart_num) + F101_UART_LSR) & F101_UART_LSR_TEMT)) {
        if (ticks_to_wait != portMAX_DELAY && (int32_t)(end - xTaskGetTickCount()) <= 0) {
            return ESP_ERR_TIMEOUT;
        }
    }
    return ESP_OK;
}

esp_err_t uart_flush(uart_port_t uart_num)
{
    return uart_wait_tx_done(uart_num, portMAX_DELAY);
}

int uart_tx_chars(uart_port_t uart_num, const char *buffer, uint32_t len)
{
    ESP_RETURN_ON_FALSE(uart_num >= 0 && uart_num < NUM_UARTS && buffer, -1, TAG, "invalid argument");
    uint32_t b = base(uart_num);
    uint32_t n = 0;

    /* THRE means the FIFO has room for at least one byte */
    while (n < len && (F101_REG32(b + F101_UART_LSR) & F101_UART_LSR_THRE)) {
        F101_REG32(b + F101_UART_RBR_THR_DLL) = (uint8_t)buffer[n++];
    }
    return (int)n;
}

int uart_write_bytes(uart_port_t uart_num, const void *src, size_t size)
{
    ESP_RETURN_ON_FALSE(uart_num >= 0 && uart_num < NUM_UARTS && src, -1, TAG, "invalid argument");
    const uint8_t *p = src;

    for (size_t i = 0; i < size; i++) {
        f101_uart_putc(uart_num, (char)p[i]);
    }
    return (int)size;
}

int uart_read_bytes(uart_port_t uart_num, void *buf, uint32_t length, uint32_t ticks_to_wait)
{
    ESP_RETURN_ON_FALSE(uart_num >= 0 && uart_num < NUM_UARTS && s_uart[uart_num].installed && buf, -1, TAG,
                        "invalid argument");
    return (int)xStreamBufferReceive(s_uart[uart_num].rx, buf, length, ticks_to_wait);
}

esp_err_t uart_flush_input(uart_port_t uart_num)
{
    CHECK_INST(uart_num);
    uint32_t b = base(uart_num);

    F101_REG32(b + F101_UART_FCR_IIR) = 0x07 | (2u << 6);
    xStreamBufferReset(s_uart[uart_num].rx);
    return ESP_OK;
}

esp_err_t uart_get_buffered_data_len(uart_port_t uart_num, size_t *size)
{
    CHECK_INST(uart_num);
    ESP_RETURN_ON_FALSE(size, ESP_ERR_INVALID_ARG, TAG, "null pointer");
    *size = xStreamBufferBytesAvailable(s_uart[uart_num].rx);
    return ESP_OK;
}

esp_err_t uart_get_tx_buffer_free_size(uart_port_t uart_num, size_t *size)
{
    CHECK_PORT(uart_num);
    *size = 128;
    return ESP_OK;
}
