/*
 * SPDX-FileCopyrightText: 2026 Yuzuki Tsuru
 * SPDX-License-Identifier: Apache-2.0
 *
 * driver/ledc.h on the four PWM channels. The hardware has one period
 * generator per channel, so a LEDC timer only stores frequency and duty
 * resolution and every channel using it is programmed with them.
 */

#include <string.h>
#include "sdkconfig.h"
#include "esp_check.h"
#include "esp_log.h"
#include "driver/ledc.h"
#include "soc/sun252i_f101_ll.h"
#include "freertos/FreeRTOS.h"

#define PWM_BASE        0x02000c00u
#define PWM_BGR         (F101_CCU_BASE + 0x7acu)
#define PWM_PCCR(ch)    (0x20u + ((ch) / 2) * 4u)
#define PWM_PCGR        0x40u
#define PWM_PER         0x80u
#define PWM_PCR(ch)     (0x100u + (ch) * 0x20u)
#define PWM_PPR(ch)     (0x104u + (ch) * 0x20u)
#define REF_HZ          24000000ull
#define FAST_HZ         100000000ull
#define NUM_CH          4
#define NUM_TIMERS      4

static const char *TAG = "ledc";

static struct {
    bool configured;
    uint32_t freq_hz;
    int res_bits;
} s_timer[NUM_TIMERS];

static struct {
    bool configured;
    int timer;
    uint32_t duty;      /* committed duty */
    uint32_t next;      /* duty set but not yet updated */
    int gpio;
    bool running;
} s_ch[NUM_CH];

static bool s_hw_init;

static inline uint32_t rd(uint32_t off)
{
    return F101_REG32(PWM_BASE + off);
}

static inline void wr(uint32_t off, uint32_t v)
{
    F101_REG32(PWM_BASE + off) = v;
}

static void hw_init(void)
{
    if (s_hw_init) {
        return;
    }
    F101_REG32(PWM_BGR) &= ~(1u << 16);
    F101_REG32(PWM_BGR) |= 1u;
    F101_REG32(PWM_BGR) |= (1u << 16);
    wr(PWM_PER, 0);
    wr(PWM_PCGR, 0);
    s_hw_init = true;
}

/* pad function of the channel pins: PD6/PD7/PD8 at mux 6, PB3 at mux 7 */
static int pin_mux_for(int ch, int gpio)
{
    static const int pins[NUM_CH] = { 3 * 32 + 6, 3 * 32 + 7, 3 * 32 + 8, 1 * 32 + 3 };
    static const int mux[NUM_CH] = { 6, 6, 6, 7 };

    return gpio == pins[ch] ? mux[ch] : -1;
}

static esp_err_t program(int ch)
{
    uint32_t freq = s_timer[s_ch[ch].timer].freq_hz;
    uint32_t res = 1u << s_timer[s_ch[ch].timer].res_bits;
    uint32_t duty = s_ch[ch].duty;
    uint64_t period_ns = 1000000000ull / freq;

    if (duty == 0) {
        wr(PWM_PER, rd(PWM_PER) & ~(1u << ch));
        wr(PWM_PCGR, rd(PWM_PCGR) & ~((1u << ch) | (1u << (16 + ch))));
        s_ch[ch].running = false;
        return ESP_OK;
    }
    uint64_t src = period_ns <= 334 ? FAST_HZ : REF_HZ;
    uint64_t cycles = (src * period_ns + 500000000ull) / 1000000000ull;
    uint32_t div_m = 0, prescale = 0, count = 0;

    ESP_RETURN_ON_FALSE(cycles > 0, ESP_ERR_INVALID_ARG, TAG, "frequency too high");
    for (div_m = 0; div_m <= 8 && !count; div_m++) {
        for (prescale = 0; prescale <= 255; prescale++) {
            uint64_t c = cycles / (1u << div_m) / (prescale + 1);
            if (c >= 1 && c <= 65536) {
                count = (uint32_t)c;
                break;
            }
        }
    }
    div_m--;
    ESP_RETURN_ON_FALSE(count, ESP_ERR_INVALID_ARG, TAG, "frequency too low");
    if (duty > res) {
        duty = res;
    }
    uint32_t active = (uint32_t)((uint64_t)count * duty / res);
    if (active == 0) {
        active = 1;
    }
    if (active > count) {
        active = count;
    }
    uint32_t pccr = rd(PWM_PCCR(ch));
    pccr &= ~((3u << 7) | 0xfu);
    pccr |= ((src == FAST_HZ ? 1u : 0u) << 7) | div_m;
    wr(PWM_PCCR(ch), pccr);
    wr(PWM_PCR(ch), (rd(PWM_PCR(ch)) & ~0x1ffu) | prescale);
    wr(PWM_PPR(ch), (active & 0xffffu) | ((count - 1) << 16));
    wr(PWM_PCGR, (rd(PWM_PCGR) | (1u << ch)) & ~(1u << (16 + ch)));
    wr(PWM_PER, rd(PWM_PER) | (1u << ch));
    s_ch[ch].running = true;
    return ESP_OK;
}

esp_err_t ledc_timer_config(const ledc_timer_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(cfg, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(cfg->timer_num < NUM_TIMERS, ESP_ERR_INVALID_ARG, TAG, "invalid timer");
    ESP_RETURN_ON_FALSE(cfg->freq_hz > 0 && cfg->freq_hz <= 50000000, ESP_ERR_INVALID_ARG, TAG, "invalid frequency");
    ESP_RETURN_ON_FALSE(cfg->duty_resolution >= 1 && cfg->duty_resolution <= 16, ESP_ERR_INVALID_ARG, TAG,
                        "invalid duty resolution");
    hw_init();
    s_timer[cfg->timer_num].configured = true;
    s_timer[cfg->timer_num].freq_hz = cfg->freq_hz;
    s_timer[cfg->timer_num].res_bits = cfg->duty_resolution;
    for (int c = 0; c < NUM_CH; c++) {
        if (s_ch[c].configured && s_ch[c].timer == (int)cfg->timer_num && s_ch[c].running) {
            program(c);
        }
    }
    return ESP_OK;
}

esp_err_t ledc_channel_config(const ledc_channel_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(cfg, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(cfg->channel < NUM_CH, ESP_ERR_INVALID_ARG, TAG, "invalid channel");
    ESP_RETURN_ON_FALSE(cfg->timer_sel < NUM_TIMERS && s_timer[cfg->timer_sel].configured, ESP_ERR_INVALID_STATE, TAG,
                        "timer not configured");
    int mux = pin_mux_for(cfg->channel, cfg->gpio_num);
    ESP_RETURN_ON_FALSE(mux >= 0, ESP_ERR_INVALID_ARG, TAG, "channel %d is not routed to this gpio", cfg->channel);
    hw_init();
    int c = cfg->channel;
    s_ch[c].configured = true;
    s_ch[c].timer = cfg->timer_sel;
    s_ch[c].gpio = cfg->gpio_num;
    s_ch[c].duty = s_ch[c].next = cfg->duty;
    f101_pin_mux(cfg->gpio_num, mux);
    return program(c);
}

esp_err_t ledc_set_pin(int gpio_num, ledc_mode_t speed_mode, ledc_channel_t channel)
{
    (void)speed_mode;
    ESP_RETURN_ON_FALSE(channel < NUM_CH, ESP_ERR_INVALID_ARG, TAG, "invalid channel");
    int mux = pin_mux_for(channel, gpio_num);
    ESP_RETURN_ON_FALSE(mux >= 0, ESP_ERR_INVALID_ARG, TAG, "channel not routed to this gpio");
    f101_pin_mux(gpio_num, mux);
    s_ch[channel].gpio = gpio_num;
    return ESP_OK;
}

esp_err_t ledc_set_duty(ledc_mode_t speed_mode, ledc_channel_t channel, uint32_t duty)
{
    (void)speed_mode;
    ESP_RETURN_ON_FALSE(channel < NUM_CH && s_ch[channel].configured, ESP_ERR_INVALID_ARG, TAG, "invalid channel");
    ESP_RETURN_ON_FALSE(duty <= (1u << s_timer[s_ch[channel].timer].res_bits), ESP_ERR_INVALID_ARG, TAG,
                        "duty out of range");
    s_ch[channel].next = duty;
    return ESP_OK;
}

esp_err_t ledc_update_duty(ledc_mode_t speed_mode, ledc_channel_t channel)
{
    (void)speed_mode;
    ESP_RETURN_ON_FALSE(channel < NUM_CH && s_ch[channel].configured, ESP_ERR_INVALID_ARG, TAG, "invalid channel");
    s_ch[channel].duty = s_ch[channel].next;
    return program(channel);
}

uint32_t ledc_get_duty(ledc_mode_t speed_mode, ledc_channel_t channel)
{
    (void)speed_mode;
    return channel < NUM_CH ? s_ch[channel].duty : LEDC_ERR_DUTY;
}

esp_err_t ledc_set_freq(ledc_mode_t speed_mode, ledc_timer_t timer_num, uint32_t freq_hz)
{
    (void)speed_mode;
    ESP_RETURN_ON_FALSE(timer_num < NUM_TIMERS && s_timer[timer_num].configured && freq_hz > 0, ESP_ERR_INVALID_ARG,
                        TAG, "invalid argument");
    s_timer[timer_num].freq_hz = freq_hz;
    for (int c = 0; c < NUM_CH; c++) {
        if (s_ch[c].configured && s_ch[c].timer == (int)timer_num && s_ch[c].running) {
            program(c);
        }
    }
    return ESP_OK;
}

uint32_t ledc_get_freq(ledc_mode_t speed_mode, ledc_timer_t timer_num)
{
    (void)speed_mode;
    return timer_num < NUM_TIMERS ? s_timer[timer_num].freq_hz : 0;
}

esp_err_t ledc_stop(ledc_mode_t speed_mode, ledc_channel_t channel, uint32_t idle_level)
{
    (void)speed_mode;
    ESP_RETURN_ON_FALSE(channel < NUM_CH && s_ch[channel].configured, ESP_ERR_INVALID_ARG, TAG, "invalid channel");
    uint32_t keep = s_ch[channel].duty;

    s_ch[channel].duty = 0;
    program(channel);
    s_ch[channel].duty = keep;
    /* the pad goes back to a plain output holding the idle level */
    f101_gpio_set_level(s_ch[channel].gpio, idle_level != 0);
    f101_pin_mux(s_ch[channel].gpio, F101_GPIO_OUT);
    return ESP_OK;
}
