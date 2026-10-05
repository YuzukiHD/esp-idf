/*
 * SPDX-FileCopyrightText: 2026 Yuzuki Tsuru
 * SPDX-License-Identifier: Apache-2.0
 *
 * On-chip audio codec at 0x02030000: digital DAC (TX) and ADC (RX) FIFOs, the analog output and input
 * stage, and the PLL_AUDIO1 / module clocks they run from. Two sample rate families exist and need
 * their own PLL setting: 8/12/16/24/32/48/96/192 kHz (PLL 3.072 GHz, divided by 5) and 11.025 /
 * 22.05 / 44.1 / 88.2 / 176.4 kHz (PLL 2.1676 GHz fractional, divided by 2). The PLL is shared, so
 * the DAC and the ADC have to be in the same family at a time.
 */

#include <string.h>
#include "sdkconfig.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "driver/gpio.h"
#include "driver/i2s_f101_codec.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "soc/sun252i_f101_ll.h"
#include "codec_f101_priv.h"

static const char *TAG = "codec";

#define CODEC_BASE          0x02030000u
#define CODEC_BUS_GATE      (F101_CCU_BASE + 0xa5cu)        /* bit 0 gate, bit 16 reset */
#define DAC_CLK_REG         (F101_CCU_BASE + 0xa50u)
#define ADC_CLK_REG         (F101_CCU_BASE + 0xa54u)

/* digital registers */
#define DAC_DPC             0x00u
#define DAC_VOL_CTL         0x04u
#define DAC_FIFO_CTL        0x10u
#define DAC_FIFO_STA        0x14u
#define DAC_TXDATA          0x20u
#define DAC_CNT             0x24u
#define ADC_FIFO_CTL        0x30u
#define ADC_VOL_CTL         0x34u
#define ADC_FIFO_STA        0x38u
#define ADC_RXDATA          0x40u
#define ADC_CNT             0x44u
#define ADC_DIG_CTL         0x50u

#define DAC_DIG_EN          (1u << 31)
#define DAC_DVOL_MASK       (0x3fu << 12)
#define DAC_VOL_SEL         (1u << 16)
#define DAC_FS_SHIFT        29
#define DAC_FIFO_MODE_SHIFT 24
#define DAC_MONO_EN         (1u << 6)
#define DAC_TX_SAMPLE_24    (1u << 5)
#define DAC_DRQ_EN          (1u << 4)
#define DAC_FIFO_FLUSH      (1u << 0)
#define DAC_STA_CLEAR       ((1u << 3) | (1u << 2) | (1u << 1))

#define ADC_FS_SHIFT        29
#define ADC_EN              (1u << 28)
#define ADC_RX_FIFO_16      (1u << 24)
#define ADC_RX_SAMPLE_24    (1u << 16)
#define ADC_DRQ_EN          (1u << 3)
#define ADC_FIFO_FLUSH      (1u << 0)
#define ADC_STA_CLEAR       ((1u << 3) | (1u << 1))
#define ADC1_CHANNEL_EN     (1u << 0)

/* analog registers */
#define ADC1_AN             0x300u
#define ADC1_EN             (1u << 31)
#define MIC1_PGA_EN         (1u << 30)
#define FMINLEN             (1u << 27)
#define LINEINLEN           (1u << 23)
#define LINEINL_GAIN_SHIFT  18
#define FML_GAIN_SHIFT      16
#define ADC1_PGA_GAIN_SHIFT 8
#define DAC_AN              0x310u
#define DACL_EN             (1u << 15)
#define DACR_EN             (1u << 14)
#define LMUTE               (1u << 12)
#define RMUTE               (1u << 10)
#define RAMP                0x31cu
#define RMC_EN              (1u << 1)
#define RK_OPT_EN           (1u << 21)
#define HP2                 0x340u
#define HPFB_BUF_EN         (1u << 31)
#define HP_GAIN_SHIFT       28
#define HP_GAIN_MASK        (0x7u << HP_GAIN_SHIFT)
#define HP_DRVEN            (1u << 21)
#define RSWITCH             (1u << 19)
#define HPFB_IN_EN          (1u << 17)
#define RAMP_OUT_EN         (1u << 15)

/* the speaker amplifier of the board is enabled by PE10 */
#define PA_GPIO             138
#define PA_DELAY_MS         50

/* PLL_AUDIO1: N[15:8] (multiplier N + 1), /2 output [18:16], /5 output [22:20] */
#define PLL_AUDIO1_REG      (F101_CCU_BASE + 0x80u)
#define PLL_AUDIO1_PAT0     (F101_CCU_BASE + 0x180u)
#define PLL_AUDIO1_PAT1     (F101_CCU_BASE + 0x184u)
#define PLL_SDM_EN          (1u << 24)
#define PLL_OUT             (1u << 27)
#define PLL_LOCKED          (1u << 28)
#define PLL_LOCK_EN         (1u << 29)
#define PLL_LDO             (1u << 30)
#define PLL_EN              (1u << 31)
#define PLL_N_SHIFT         8
#define PLL_DIV2_SHIFT      16
#define PLL_DIV5_SHIFT      20
#define PLL_DIV_MASK        0x7u
#define PLL_PAT0_44K1       0xc000a234u
#define HOSC_HZ             24000000u
#define PLL_48K_N           128u            /* 24 MHz * 128 = 3.072 GHz, /5 = 614.4 MHz */
#define PLL_48K_DIV         5u
#define PLL_44K1_N          90u             /* 2.160 GHz plus the fractional part */
#define PLL_44K1_RATE       2167603200u
#define PLL_44K1_DIV        2u              /* 1083.8016 MHz */

#define MOD_GATE            (1u << 31)

typedef enum {
    FAMILY_48K,
    FAMILY_44K1,
} family_t;

static SemaphoreHandle_t s_lock;
static int s_pll_users;
static family_t s_pll_family;
static bool s_clk_held[2];
static bool s_inited;
static bool s_output_on;
static bool s_muted;
static int s_volume = 130;              /* register value 0..255 */
static int s_adc_volume = 160;
static int s_hp_gain = 7;
static int s_mic_gain = 15;
static i2s_f101_input_t s_input = I2S_F101_INPUT_MIC;

static inline uint32_t rd(uint32_t off)
{
    return F101_REG32(CODEC_BASE + off);
}

static inline void wr(uint32_t off, uint32_t v)
{
    F101_REG32(CODEC_BASE + off) = v;
}

static inline void upd(uint32_t off, uint32_t mask, uint32_t v)
{
    wr(off, (rd(off) & ~mask) | (v & mask));
}

/* ---- clocks ---- */

static int family_of(uint32_t rate)
{
    if (rate != 0 && rate % 8000 == 0) {
        return FAMILY_48K;
    }
    if (rate != 0 && rate % 11025 == 0) {
        return FAMILY_44K1;
    }
    return -1;
}

static uint32_t base_mclk(family_t f)
{
    return f == FAMILY_48K ? 24576000u : 22579200u;
}

static uint32_t pll_out_rate(family_t f)
{
    return f == FAMILY_48K ? HOSC_HZ * PLL_48K_N / PLL_48K_DIV : PLL_44K1_RATE / PLL_44K1_DIV;
}

static esp_err_t pll_start(family_t f)
{
    uint32_t reg = F101_REG32(PLL_AUDIO1_REG);
    uint32_t n = f == FAMILY_48K ? PLL_48K_N : PLL_44K1_N;

    /* reprogram with the output gated and the PLL off */
    reg &= ~(PLL_OUT | PLL_EN | PLL_LDO | PLL_LOCK_EN | PLL_SDM_EN | (0xffu << PLL_N_SHIFT) |
             (PLL_DIV_MASK << PLL_DIV2_SHIFT) | (PLL_DIV_MASK << PLL_DIV5_SHIFT));
    F101_REG32(PLL_AUDIO1_REG) = reg;
    reg |= ((n - 1) << PLL_N_SHIFT) | ((PLL_44K1_DIV - 1) << PLL_DIV2_SHIFT) | ((PLL_48K_DIV - 1) << PLL_DIV5_SHIFT);
    if (f == FAMILY_44K1) {
        F101_REG32(PLL_AUDIO1_PAT0) = PLL_PAT0_44K1;
        F101_REG32(PLL_AUDIO1_PAT1) = 0;
        reg |= PLL_SDM_EN;
    } else {
        F101_REG32(PLL_AUDIO1_PAT0) = 0;
        F101_REG32(PLL_AUDIO1_PAT1) = 0;
    }
    F101_REG32(PLL_AUDIO1_REG) = reg;
    reg |= PLL_LDO;
    F101_REG32(PLL_AUDIO1_REG) = reg;
    reg |= PLL_EN;
    F101_REG32(PLL_AUDIO1_REG) = reg;
    reg |= PLL_LOCK_EN;
    F101_REG32(PLL_AUDIO1_REG) = reg;
    for (int tries = 2000; tries > 0 && !(F101_REG32(PLL_AUDIO1_REG) & PLL_LOCKED); tries--) {
        esp_rom_delay_us(1);
    }
    if (!(F101_REG32(PLL_AUDIO1_REG) & PLL_LOCKED)) {
        ESP_LOGE(TAG, "PLL_AUDIO1 does not lock (%08x)", (unsigned)F101_REG32(PLL_AUDIO1_REG));
        F101_REG32(PLL_AUDIO1_REG) = reg & ~(PLL_EN | PLL_LDO | PLL_LOCK_EN);
        return ESP_ERR_TIMEOUT;
    }
    F101_REG32(PLL_AUDIO1_REG) = reg | PLL_OUT;
    return ESP_OK;
}

static void pll_stop(void)
{
    uint32_t reg = F101_REG32(PLL_AUDIO1_REG) & ~PLL_OUT;

    F101_REG32(PLL_AUDIO1_REG) = reg;
    F101_REG32(PLL_AUDIO1_REG) = reg & ~(PLL_EN | PLL_LDO | PLL_LOCK_EN | PLL_SDM_EN);
}

/* called with s_lock held */
static esp_err_t pll_get(family_t f)
{
    if (s_pll_users == 0) {
        ESP_RETURN_ON_ERROR(pll_start(f), TAG, "pll");
        s_pll_family = f;
        s_pll_users = 1;
    } else if (s_pll_family != f) {
        return ESP_ERR_INVALID_STATE;       /* the other direction runs in the other family */
    } else {
        s_pll_users++;
    }
    return ESP_OK;
}

static void pll_put(void)
{
    if (s_pll_users > 0 && --s_pll_users == 0) {
        pll_stop();
    }
}

/* module clock register: M[4:0], P[9:8], source[26:24] (0: PLL/2, 1: PLL/5), gate 31 */
static esp_err_t module_clk_set(uint32_t reg, uint32_t rate)
{
    uint32_t src = pll_out_rate(s_pll_family);
    uint32_t div = (src + rate / 2) / rate;
    uint32_t best_p = 0, best_m = 1, best_err = UINT32_MAX;

    for (uint32_t p = 0; p < 4; p++) {
        uint32_t m = (div + (1u << p) / 2) >> p;

        if (m < 1 || m > 32) {
            continue;
        }
        uint32_t got = src / (m << p);
        uint32_t err = got > rate ? got - rate : rate - got;

        if (err < best_err) {
            best_err = err;
            best_p = p;
            best_m = m;
        }
    }
    ESP_RETURN_ON_FALSE(best_err <= rate / 100, ESP_ERR_INVALID_ARG, TAG, "clock %u Hz is not reachable", (unsigned)rate);
    F101_REG32(reg) = 0;                    /* the gate stays closed while the divider changes */
    F101_REG32(reg) = ((s_pll_family == FAMILY_48K ? 1u : 0u) << 24) | (best_p << 8) | (best_m - 1);
    F101_REG32(reg) |= MOD_GATE;
    return ESP_OK;
}

/* ---- analog front end ---- */

static void apply_mute(void)
{
    upd(DAC_AN, LMUTE | RMUTE, (s_output_on && !s_muted) ? (LMUTE | RMUTE) : 0);
}

static void apply_volume(void)
{
    upd(DAC_VOL_CTL, 0xffu | (0xffu << 8), ((uint32_t)s_volume << 8) | (uint32_t)s_volume);
}

static void apply_source(void)
{
    uint32_t v = 0;

    switch (s_input) {
    case I2S_F101_INPUT_MIC:
        v = ADC1_EN | MIC1_PGA_EN;
        break;
    case I2S_F101_INPUT_FMIN:
        v = ADC1_EN | FMINLEN;
        break;
    case I2S_F101_INPUT_LINEIN:
        v = ADC1_EN | LINEINLEN;
        break;
    default:
        break;
    }
    upd(ADC1_AN, ADC1_EN | MIC1_PGA_EN | FMINLEN | LINEINLEN, v);
}

static void output_on(void)
{
    if (s_output_on) {
        return;
    }
    s_output_on = true;
    apply_mute();
    upd(DAC_DPC, DAC_DIG_EN, DAC_DIG_EN);
    upd(DAC_AN, DACL_EN | DACR_EN, DACL_EN | DACR_EN);
    upd(HP2, HPFB_BUF_EN | HPFB_IN_EN, HPFB_BUF_EN | HPFB_IN_EN);
    upd(HP2, RAMP_OUT_EN | RSWITCH, RAMP_OUT_EN | RSWITCH);
    upd(HP2, HP_DRVEN, HP_DRVEN);
    gpio_set_level(PA_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(PA_DELAY_MS));
}

static void output_off(void)
{
    if (!s_output_on) {
        return;
    }
    gpio_set_level(PA_GPIO, 0);
    upd(HP2, HP_DRVEN, 0);
    s_output_on = false;
    apply_mute();
    upd(HP2, RAMP_OUT_EN | RSWITCH, 0);
    upd(HP2, HPFB_BUF_EN | HPFB_IN_EN, 0);
    upd(DAC_DPC, DAC_DIG_EN, 0);
    upd(DAC_AN, DACL_EN | DACR_EN, 0);
}

esp_err_t codec_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "no memory");

    gpio_set_level(PA_GPIO, 0);
    ESP_RETURN_ON_ERROR(gpio_set_direction(PA_GPIO, GPIO_MODE_OUTPUT), TAG, "pa gpio");

    F101_REG32(CODEC_BUS_GATE) &= ~(1u << 16);                  /* reset */
    F101_REG32(CODEC_BUS_GATE) |= (1u << 0);
    F101_REG32(CODEC_BUS_GATE) |= (1u << 16);

    /* volume control on, drop the first samples of a capture while the input settles */
    upd(ADC_DIG_CTL, 1u << 16, 1u << 16);
    upd(0x04, 1u << 16, 1u << 16);
    upd(ADC_FIFO_CTL, (1u << 25) | (3u << 26), (1u << 25) | (2u << 26));

    /* manual ramp control and no pop when the headphone driver powers up */
    upd(RAMP, RMC_EN | RK_OPT_EN, RMC_EN | RK_OPT_EN);
    upd(DAC_VOL_CTL, DAC_VOL_SEL, DAC_VOL_SEL);
    upd(DAC_DPC, DAC_DVOL_MASK, 0);
    upd(HP2, HP_GAIN_MASK, (uint32_t)(7 - s_hp_gain) << HP_GAIN_SHIFT);
    apply_volume();
    upd(ADC_VOL_CTL, 0xffu, (uint32_t)s_adc_volume);
    upd(ADC1_AN, 0xfu << ADC1_PGA_GAIN_SHIFT, (uint32_t)s_mic_gain << ADC1_PGA_GAIN_SHIFT);
    upd(ADC1_AN, (3u << LINEINL_GAIN_SHIFT) | (3u << FML_GAIN_SHIFT), (3u << LINEINL_GAIN_SHIFT) | (3u << FML_GAIN_SHIFT));
    apply_source();
    s_inited = true;
    return ESP_OK;
}

static int rate_code(uint32_t rate)
{
    switch (rate) {
    case 48000:  return 0;
    case 32000:  return 1;
    case 24000:  return 2;
    case 16000:  return 3;
    case 12000:  return 4;
    case 8000:   return 5;
    case 192000: return 6;
    case 96000:  return 7;
    default:     return -1;
    }
}

esp_err_t codec_configure(codec_dir_t dir, uint32_t rate_hz, uint8_t data_bytes, uint8_t channels)
{
    bool tx = dir == CODEC_DIR_TX;
    int fam = family_of(rate_hz);
    uint32_t norm = rate_hz;

    ESP_RETURN_ON_FALSE(fam >= 0, ESP_ERR_INVALID_ARG, TAG, "sample rate %u Hz is not supported", (unsigned)rate_hz);
    if (fam == FAMILY_44K1) {
        norm = (uint32_t)(((uint64_t)rate_hz * 48000u + 22050u) / 44100u);     /* the same dividers from a 22.5792 MHz clock */
    }
    int code = rate_code(norm);
    ESP_RETURN_ON_FALSE(code >= 0, ESP_ERR_INVALID_ARG, TAG, "sample rate %u Hz is not supported", (unsigned)rate_hz);
    ESP_RETURN_ON_FALSE(tx || !(code > 0 && norm > 48000), ESP_ERR_INVALID_ARG, TAG, "the ADC runs up to 48 kHz");
    ESP_RETURN_ON_FALSE(data_bytes == 2 || data_bytes == 4, ESP_ERR_INVALID_ARG, TAG, "16 or 32 bit samples");
    ESP_RETURN_ON_FALSE(channels >= 1 && channels <= (tx ? 2 : 1), ESP_ERR_INVALID_ARG, TAG, "channels");

    esp_err_t err = ESP_OK;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_clk_held[dir]) {
        F101_REG32(tx ? DAC_CLK_REG : ADC_CLK_REG) &= ~MOD_GATE;
        pll_put();
        s_clk_held[dir] = false;
    }
    err = pll_get((family_t)fam);
    if (err == ESP_OK) {
        err = module_clk_set(tx ? DAC_CLK_REG : ADC_CLK_REG, base_mclk((family_t)fam));
        if (err == ESP_OK) {
            s_clk_held[dir] = true;
        } else {
            pll_put();
        }
    }
    xSemaphoreGive(s_lock);
    ESP_RETURN_ON_ERROR(err, TAG, "clock");

    if (tx) {
        uint32_t v = (uint32_t)code << DAC_FS_SHIFT;

        v |= data_bytes == 2 ? (3u << DAC_FIFO_MODE_SHIFT) : DAC_TX_SAMPLE_24;
        if (channels == 1) {
            v |= DAC_MONO_EN;
        }
        upd(DAC_FIFO_CTL, (7u << DAC_FS_SHIFT) | (3u << DAC_FIFO_MODE_SHIFT) | DAC_TX_SAMPLE_24 | DAC_MONO_EN, v);
    } else {
        uint32_t v = (uint32_t)code << ADC_FS_SHIFT;

        v |= data_bytes == 2 ? ADC_RX_FIFO_16 : ADC_RX_SAMPLE_24;
        upd(ADC_FIFO_CTL, (7u << ADC_FS_SHIFT) | ADC_RX_FIFO_16 | ADC_RX_SAMPLE_24, v);
    }
    return ESP_OK;
}

void codec_release(codec_dir_t dir)
{
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_clk_held[dir]) {
        F101_REG32(dir == CODEC_DIR_TX ? DAC_CLK_REG : ADC_CLK_REG) &= ~MOD_GATE;
        pll_put();
        s_clk_held[dir] = false;
    }
    xSemaphoreGive(s_lock);
}

uint32_t codec_fifo_addr(codec_dir_t dir)
{
    return CODEC_BASE + (dir == CODEC_DIR_TX ? DAC_TXDATA : ADC_RXDATA);
}

void codec_start(codec_dir_t dir)
{
    if (dir == CODEC_DIR_TX) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        output_on();
        xSemaphoreGive(s_lock);
        upd(DAC_FIFO_CTL, DAC_FIFO_FLUSH, DAC_FIFO_FLUSH);
        wr(DAC_FIFO_STA, DAC_STA_CLEAR);
        wr(DAC_CNT, 0);
        upd(DAC_FIFO_CTL, DAC_DRQ_EN, DAC_DRQ_EN);
    } else {
        upd(ADC_DIG_CTL, ADC1_CHANNEL_EN, ADC1_CHANNEL_EN);
        upd(ADC_FIFO_CTL, ADC_FIFO_FLUSH | ADC_EN, ADC_FIFO_FLUSH | ADC_EN);
        wr(ADC_FIFO_STA, ADC_STA_CLEAR);
        wr(ADC_CNT, 0);
        upd(ADC_FIFO_CTL, ADC_DRQ_EN, ADC_DRQ_EN);
    }
}

void codec_stop(codec_dir_t dir)
{
    if (dir == CODEC_DIR_TX) {
        upd(DAC_FIFO_CTL, DAC_DRQ_EN, 0);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        output_off();
        xSemaphoreGive(s_lock);
    } else {
        upd(ADC_FIFO_CTL, ADC_DRQ_EN | ADC_EN, 0);
        upd(ADC_DIG_CTL, ADC1_CHANNEL_EN, 0);
    }
}

/* ---- analog controls of the public header ---- */

#define CHECK_INIT() ESP_RETURN_ON_FALSE(s_inited, ESP_ERR_INVALID_STATE, TAG, "no I2S channel was created yet")

esp_err_t i2s_f101_codec_set_volume(int percent)
{
    CHECK_INIT();
    ESP_RETURN_ON_FALSE(percent >= 0 && percent <= 100, ESP_ERR_INVALID_ARG, TAG, "0..100");
    s_volume = percent * 255 / 100;
    apply_volume();
    return ESP_OK;
}

esp_err_t i2s_f101_codec_set_mute(bool mute)
{
    CHECK_INIT();
    s_muted = mute;
    apply_mute();
    return ESP_OK;
}

esp_err_t i2s_f101_codec_set_hp_gain(int gain)
{
    CHECK_INIT();
    ESP_RETURN_ON_FALSE(gain >= 0 && gain <= 7, ESP_ERR_INVALID_ARG, TAG, "0..7");
    s_hp_gain = gain;
    upd(HP2, HP_GAIN_MASK, (uint32_t)(7 - gain) << HP_GAIN_SHIFT);
    return ESP_OK;
}

esp_err_t i2s_f101_codec_set_input_volume(int percent)
{
    CHECK_INIT();
    ESP_RETURN_ON_FALSE(percent >= 0 && percent <= 100, ESP_ERR_INVALID_ARG, TAG, "0..100");
    s_adc_volume = percent * 255 / 100;
    upd(ADC_VOL_CTL, 0xffu, (uint32_t)s_adc_volume);
    return ESP_OK;
}

esp_err_t i2s_f101_codec_set_mic_gain(int gain)
{
    CHECK_INIT();
    ESP_RETURN_ON_FALSE(gain >= 0 && gain <= 15, ESP_ERR_INVALID_ARG, TAG, "0..15");
    s_mic_gain = gain;
    upd(ADC1_AN, 0xfu << ADC1_PGA_GAIN_SHIFT, (uint32_t)gain << ADC1_PGA_GAIN_SHIFT);
    return ESP_OK;
}

esp_err_t i2s_f101_codec_select_input(i2s_f101_input_t input)
{
    CHECK_INIT();
    ESP_RETURN_ON_FALSE(input >= I2S_F101_INPUT_NONE && input <= I2S_F101_INPUT_LINEIN, ESP_ERR_INVALID_ARG, TAG, "input");
    s_input = input;
    apply_source();
    return ESP_OK;
}
