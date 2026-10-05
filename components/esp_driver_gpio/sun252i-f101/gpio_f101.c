/*
 * SPDX-FileCopyrightText: 2026 Yuzuki Tsuru
 * SPDX-License-Identifier: Apache-2.0
 *
 * driver/gpio.h on the PIO banks. Pin numbers are bank * 32 + pin. Pin
 * interrupts use the per bank PIO interrupt block (pin function 0xe) and the
 * bank interrupt line of the PLIC.
 */

#include <string.h>
#include <stdlib.h>
#include "sdkconfig.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_intr_alloc.h"
#include "driver/gpio.h"
#include "soc/sun252i_f101_ll.h"
#include "freertos/FreeRTOS.h"

#define NUM_BANKS       6
#define PIN_COUNT       (NUM_BANKS * 32)
#define IRQ_BASE(bank)  (F101_PIO_BASE + 0x200u + (bank) * 0x20u)
#define IRQ_CTL         0x10u
#define IRQ_STATUS      0x14u
#define IRQ_MUX         0xeu
#define IRQ_DEBOUNCE    0x18u
#define BANK_PLIC_SRC(bank) (67 + (bank) * 2)

static const char *TAG = "gpio";

typedef struct {
    gpio_isr_t fn;
    void *arg;
    uint8_t prev_mux;
    uint8_t intr_type;
} pin_irq_t;

static pin_irq_t s_pin[PIN_COUNT];
static intr_handle_t s_bank_handle[NUM_BANKS];
static bool s_isr_service;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

#define CHECK_PIN(p) ESP_RETURN_ON_FALSE((p) >= 0 && (p) < PIN_COUNT, ESP_ERR_INVALID_ARG, TAG, "invalid gpio number")

static uint32_t get_mux(int pin)
{
    uint32_t r = f101_pio_bank(pin) + (uint32_t)((pin % 32) / 8) * 4;

    return (F101_REG32(r) >> ((pin % 8) * 4)) & 0xf;
}

static void bank_isr(void *arg)
{
    int bank = (int)(intptr_t)arg;
    uint32_t b = IRQ_BASE(bank);
    uint32_t st = F101_REG32(b + IRQ_STATUS);

    F101_REG32(b + IRQ_STATUS) = st;
    while (st) {
        int n = __builtin_ctz(st);
        st &= st - 1;
        pin_irq_t *p = &s_pin[bank * 32 + n];
        if (p->fn) {
            p->fn(p->arg);
        }
    }
}

static esp_err_t bank_irq_get(int bank)
{
    if (s_bank_handle[bank]) {
        return ESP_OK;
    }
    return esp_intr_alloc(BANK_PLIC_SRC(bank), ESP_INTR_FLAG_LEVEL1, bank_isr, (void *)(intptr_t)bank,
                          &s_bank_handle[bank]);
}

esp_err_t gpio_set_direction(gpio_num_t gpio_num, gpio_mode_t mode)
{
    CHECK_PIN(gpio_num);
    if ((mode & GPIO_MODE_DEF_OUTPUT) && (mode & GPIO_MODE_DEF_INPUT)) {
        /* the PIO has no combined input/output state, output also reads back */
    }
    f101_pin_mux(gpio_num, (mode & GPIO_MODE_DEF_OUTPUT) ? F101_GPIO_OUT : F101_GPIO_IN);
    return ESP_OK;
}

esp_err_t gpio_input_enable(gpio_num_t gpio_num)
{
    CHECK_PIN(gpio_num);
    f101_pin_mux(gpio_num, F101_GPIO_IN);
    return ESP_OK;
}

esp_err_t gpio_output_enable(gpio_num_t gpio_num)
{
    CHECK_PIN(gpio_num);
    f101_pin_mux(gpio_num, F101_GPIO_OUT);
    return ESP_OK;
}

esp_err_t gpio_output_disable(gpio_num_t gpio_num)
{
    return gpio_input_enable(gpio_num);
}

esp_err_t gpio_set_level(gpio_num_t gpio_num, uint32_t level)
{
    CHECK_PIN(gpio_num);
    f101_gpio_set_level(gpio_num, level != 0);
    return ESP_OK;
}

int gpio_get_level(gpio_num_t gpio_num)
{
    if (gpio_num < 0 || gpio_num >= PIN_COUNT) {
        return 0;
    }
    return f101_gpio_get_level(gpio_num);
}

esp_err_t gpio_set_pull_mode(gpio_num_t gpio_num, gpio_pull_mode_t pull)
{
    CHECK_PIN(gpio_num);
    switch (pull) {
    case GPIO_PULLUP_ONLY:
        f101_pin_pull(gpio_num, F101_PULL_UP);
        break;
    case GPIO_PULLDOWN_ONLY:
        f101_pin_pull(gpio_num, F101_PULL_DOWN);
        break;
    case GPIO_FLOATING:
        f101_pin_pull(gpio_num, F101_PULL_NONE);
        break;
    default:
        return ESP_ERR_INVALID_ARG;     /* no combined pull up and down */
    }
    return ESP_OK;
}

esp_err_t gpio_pullup_en(gpio_num_t gpio_num)
{
    return gpio_set_pull_mode(gpio_num, GPIO_PULLUP_ONLY);
}

esp_err_t gpio_pullup_dis(gpio_num_t gpio_num)
{
    return gpio_set_pull_mode(gpio_num, GPIO_FLOATING);
}

esp_err_t gpio_pulldown_en(gpio_num_t gpio_num)
{
    return gpio_set_pull_mode(gpio_num, GPIO_PULLDOWN_ONLY);
}

esp_err_t gpio_pulldown_dis(gpio_num_t gpio_num)
{
    return gpio_set_pull_mode(gpio_num, GPIO_FLOATING);
}

esp_err_t gpio_od_enable(gpio_num_t gpio_num)
{
    (void)gpio_num;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t gpio_od_disable(gpio_num_t gpio_num)
{
    (void)gpio_num;
    return ESP_OK;
}

esp_err_t gpio_reset_pin(gpio_num_t gpio_num)
{
    CHECK_PIN(gpio_num);
    gpio_intr_disable(gpio_num);
    f101_pin_pull(gpio_num, F101_PULL_NONE);
    f101_pin_mux(gpio_num, F101_GPIO_IN);
    return ESP_OK;
}

esp_err_t gpio_set_drive_capability(gpio_num_t gpio_num, gpio_drive_cap_t strength)
{
    CHECK_PIN(gpio_num);
    ESP_RETURN_ON_FALSE(strength < GPIO_DRIVE_CAP_MAX, ESP_ERR_INVALID_ARG, TAG, "invalid strength");
    f101_pin_drive(gpio_num, (uint32_t)strength);
    return ESP_OK;
}

esp_err_t gpio_get_drive_capability(gpio_num_t gpio_num, gpio_drive_cap_t *strength)
{
    CHECK_PIN(gpio_num);
    ESP_RETURN_ON_FALSE(strength, ESP_ERR_INVALID_ARG, TAG, "null pointer");
    uint32_t r = f101_pio_bank(gpio_num) + 0x14u + (uint32_t)((gpio_num % 32) / 16) * 4;

    *strength = (gpio_drive_cap_t)((F101_REG32(r) >> ((gpio_num % 16) * 2)) & 3);
    return ESP_OK;
}

esp_err_t gpio_config(const gpio_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(cfg && cfg->pin_bit_mask, ESP_ERR_INVALID_ARG, TAG, "invalid config");
    ESP_RETURN_ON_FALSE(cfg->mode != GPIO_MODE_DISABLE || true, ESP_ERR_INVALID_ARG, TAG, "mode");

    /* the mask is 64 bits wide: PA and PB only, use gpio_set_direction() for the other banks */
    for (int pin = 0; pin < 64; pin++) {
        if (!(cfg->pin_bit_mask & (1ULL << pin))) {
            continue;
        }
        f101_pin_pull(pin, cfg->pull_up_en ? F101_PULL_UP : (cfg->pull_down_en ? F101_PULL_DOWN : F101_PULL_NONE));
        gpio_set_direction(pin, cfg->mode);
        gpio_set_intr_type(pin, cfg->intr_type);
        if (cfg->intr_type != GPIO_INTR_DISABLE) {
            gpio_intr_enable(pin);
        } else {
            gpio_intr_disable(pin);
        }
    }
    return ESP_OK;
}

/* ------------------------------ Interrupts -------------------------------- */

esp_err_t gpio_set_intr_type(gpio_num_t gpio_num, gpio_int_type_t intr_type)
{
    CHECK_PIN(gpio_num);
    ESP_RETURN_ON_FALSE(intr_type < GPIO_INTR_MAX, ESP_ERR_INVALID_ARG, TAG, "invalid intr type");
    s_pin[gpio_num].intr_type = intr_type;
    return ESP_OK;
}

static int irq_cfg_of(gpio_int_type_t t)
{
    switch (t) {
    case GPIO_INTR_POSEDGE:  return 0;
    case GPIO_INTR_NEGEDGE:  return 1;
    case GPIO_INTR_HIGH_LEVEL: return 2;
    case GPIO_INTR_LOW_LEVEL:  return 3;
    case GPIO_INTR_ANYEDGE:  return 4;
    default:                 return -1;
    }
}

esp_err_t gpio_intr_enable(gpio_num_t gpio_num)
{
    CHECK_PIN(gpio_num);
    int bank = gpio_num / 32, n = gpio_num % 32;
    int cfg = irq_cfg_of(s_pin[gpio_num].intr_type);

    ESP_RETURN_ON_FALSE(cfg >= 0, ESP_ERR_INVALID_STATE, TAG, "no interrupt type set");
    ESP_RETURN_ON_ERROR(bank_irq_get(bank), TAG, "no bank interrupt");

    portENTER_CRITICAL(&s_lock);
    /* edge detection samples with the debounce clock: take the 24 MHz oscillator, the 32 kHz one is not running */
    F101_REG32(IRQ_BASE(bank) + IRQ_DEBOUNCE) = 1;
    if (get_mux(gpio_num) != IRQ_MUX) {
        s_pin[gpio_num].prev_mux = get_mux(gpio_num);
    }
    f101_pin_mux(gpio_num, IRQ_MUX);
    uint32_t r = IRQ_BASE(bank) + (uint32_t)(n / 8) * 4;
    uint32_t sh = (uint32_t)(n % 8) * 4;
    F101_REG32(r) = (F101_REG32(r) & ~(0xfu << sh)) | ((uint32_t)cfg << sh);
    F101_REG32(IRQ_BASE(bank) + IRQ_STATUS) = 1u << n;
    F101_REG32(IRQ_BASE(bank) + IRQ_CTL) |= 1u << n;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t gpio_intr_disable(gpio_num_t gpio_num)
{
    CHECK_PIN(gpio_num);
    int bank = gpio_num / 32, n = gpio_num % 32;

    portENTER_CRITICAL(&s_lock);
    F101_REG32(IRQ_BASE(bank) + IRQ_CTL) &= ~(1u << n);
    if (get_mux(gpio_num) == IRQ_MUX) {
        f101_pin_mux(gpio_num, s_pin[gpio_num].prev_mux);
    }
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t gpio_install_isr_service(int intr_alloc_flags)
{
    (void)intr_alloc_flags;
    ESP_RETURN_ON_FALSE(!s_isr_service, ESP_ERR_INVALID_STATE, TAG, "GPIO isr service already installed");
    s_isr_service = true;
    return ESP_OK;
}

esp_err_t gpio_uninstall_isr_service(void)
{
    s_isr_service = false;
    for (int b = 0; b < NUM_BANKS; b++) {
        if (s_bank_handle[b]) {
            esp_intr_free(s_bank_handle[b]);
            s_bank_handle[b] = NULL;
        }
    }
    memset(s_pin, 0, sizeof(s_pin));
    return ESP_OK;
}

esp_err_t gpio_isr_handler_add(gpio_num_t gpio_num, gpio_isr_t isr_handler, void *args)
{
    CHECK_PIN(gpio_num);
    ESP_RETURN_ON_FALSE(s_isr_service, ESP_ERR_INVALID_STATE, TAG, "GPIO isr service is not installed, call gpio_install_isr_service() first");
    portENTER_CRITICAL(&s_lock);
    s_pin[gpio_num].fn = isr_handler;
    s_pin[gpio_num].arg = args;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t gpio_isr_handler_remove(gpio_num_t gpio_num)
{
    CHECK_PIN(gpio_num);
    portENTER_CRITICAL(&s_lock);
    s_pin[gpio_num].fn = NULL;
    s_pin[gpio_num].arg = NULL;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t gpio_isr_register(void (*fn)(void *), void *arg, int intr_alloc_flags, gpio_isr_handle_t *handle)
{
    (void)fn; (void)arg; (void)intr_alloc_flags; (void)handle;
    return ESP_ERR_NOT_SUPPORTED;       /* use gpio_install_isr_service() */
}

esp_err_t gpio_dump_io_configuration(FILE *out_stream, uint64_t io_bit_mask)
{
    for (int pin = 0; pin < 64; pin++) {
        if (io_bit_mask & (1ULL << pin)) {
            fprintf(out_stream, "P%c%02d mux=%u level=%d\n", 'A' + pin / 32, pin % 32, (unsigned)get_mux(pin),
                    gpio_get_level(pin));
        }
    }
    return ESP_OK;
}
