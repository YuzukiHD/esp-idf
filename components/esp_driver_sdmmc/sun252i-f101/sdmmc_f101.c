/*
 * SPDX-FileCopyrightText: 2026 Yuzuki Tsuru
 * SPDX-License-Identifier: Apache-2.0
 *
 * SD/MMC host (sdmmc_host_t) on the SMHC0 controller: commands and data
 * are polled, data moves through the FIFO by the CPU. The protocol layer
 * (sdmmc_card_init, sdmmc_read_sectors, ...) is the stock IDF one.
 */

#include <string.h>
#include "sdkconfig.h"
#include "esp_check.h"
#include "esp_log.h"
#include "sdmmc_cmd.h"
#include "sd_protocol_defs.h"
#include "soc/sun252i_f101_ll.h"
#include "soc/f101_periph_pins.h"
#include "driver/sdmmc_host.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_intr_alloc.h"

/* Two controllers are hosts: slot 0 is SMHC0 (the SD slot), slot 1 is SMHC2 (SDIO, the WiFi chip). The
 * functions below work on the controller named by g_c, which every public call sets from its slot
 * argument while it holds the lock. */
#define SLOTS           2
static const uint32_t k_base[SLOTS] = { 0x04020000u, 0x04022000u };
static const uint32_t k_clk_off[SLOTS] = { 0x830u, 0x838u };
static const uint8_t k_gate_bit[SLOTS] = { 0, 2 };          /* bus gate bit, the reset is 16 higher */
static const uint8_t k_inst[SLOTS] = { 0, 2 };              /* controller number in the pad tables */
static const int k_irq[SLOTS] = { 40, 42 };
static int g_c;
#define SMHC_BASE       (k_base[g_c])
#define SMHC_BGR        (F101_CCU_BASE + 0x84cu)
#define SMHC_CLK_REG    (F101_CCU_BASE + k_clk_off[g_c])
#define SMHC_GATE       (1u << k_gate_bit[g_c])
#define SMHC_RESET      (1u << (16 + k_gate_bit[g_c]))
#define PLL_PERI_REG    (F101_CCU_BASE + 0x20u)

#define GCTRL           0x00u
#define CLKCR           0x04u
#define TMOUT           0x08u
#define WIDTH           0x0cu
#define BLKSZ           0x10u
#define BCNTR           0x14u
#define CMDR            0x18u
#define CARG            0x1cu
#define RESP0           0x20u
#define IMASK           0x30u
#define RINTR           0x38u
#define STAS            0x3cu
#define FTRGL           0x40u
#define A12A            0x58u
#define NTSR            0x5cu
#define DRV_DL          0x140u
#define FIFO            0x200u

#define GCTRL_ALL_RST   0x7u
#define GCTRL_FIFO_RST  (1u << 1)
#define GCTRL_INT_EN    (1u << 4)
#define GCTRL_DDR       (1u << 10)
#define GCTRL_AHB       (1u << 31)
#define GCTRL_DONE_DIR  (1u << 30)
#define CLKCR_ON        (1u << 16)
#define CLKCR_LP        (1u << 17)
#define CLKCR_MASK_D0   (1u << 31)

#define CMD_RSP_EXP     (1u << 6)
#define CMD_LONG        (1u << 7)
#define CMD_CRC         (1u << 8)
#define CMD_DATA        (1u << 9)
#define CMD_WRITE       (1u << 10)
#define CMD_AUTOSTOP    (1u << 12)
#define CMD_WAIT_PRE    (1u << 13)
#define CMD_INIT        (1u << 15)
#define CMD_UPCLK       (1u << 21)
#define CMD_START       (1u << 31)

#define INT_CMD_DONE    (1u << 2)
#define INT_DATA_OVER   (1u << 3)
#define INT_RESP_TO     (1u << 8)
#define INT_DATA_TO     (1u << 9)
#define INT_AUTO_DONE   (1u << 14)
#define INT_ERR         ((1u << 1) | (1u << 6) | (1u << 7) | (1u << 8) | (1u << 9) | (1u << 11) | (1u << 12) | (1u << 13) | (1u << 15))
#define STAS_EMPTY      (1u << 2)
#define STAS_FULL       (1u << 3)
#define STAS_BUSY       (1u << 9)

static const char *TAG = "sdmmc_f101";

static bool s_host;                                 /* sdmmc_host_init() was called */
static bool s_init_a[SLOTS];
static bool s_slot_ready_a[SLOTS];
static size_t s_slot_width_a[SLOTS] = { 4, 4 };
static size_t s_width_a[SLOTS] = { 1, 1 };
static uint32_t s_khz_a[SLOTS];
static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_sdio_sem[SLOTS];
static intr_handle_t s_intr[SLOTS];
#define s_init          (s_init_a[g_c])
#define s_slot_ready    (s_slot_ready_a[g_c])
#define s_slot_width    (s_slot_width_a[g_c])
#define s_width         (s_width_a[g_c])
#define s_khz           (s_khz_a[g_c])

/* take the lock and make @p slot the controller the helpers address */
static esp_err_t enter(int slot)
{
    if (slot < 0 || slot >= SLOTS) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_lock) {
        s_lock = xSemaphoreCreateRecursiveMutex();
        if (!s_lock) {
            return ESP_ERR_NO_MEM;
        }
    }
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    g_c = slot;
    return ESP_OK;
}

static void leave(void)
{
    xSemaphoreGiveRecursive(s_lock);
}

static inline uint32_t rd(uint32_t off)
{
    return F101_REG32(SMHC_BASE + off);
}

static inline void wr(uint32_t off, uint32_t v)
{
    F101_REG32(SMHC_BASE + off) = v;
}

static bool wait_clear(uint32_t off, uint32_t mask, uint32_t timeout_us)
{
    uint64_t end = f101_mtime_get() + (uint64_t)timeout_us * (F101_MTIME_HZ / 1000000u);

    while (rd(off) & mask) {
        if (f101_mtime_get() > end) {
            return false;
        }
    }
    return true;
}

static esp_err_t update_clock(void)
{
    uint32_t c = rd(CLKCR);

    wr(CLKCR, c | CLKCR_MASK_D0);
    wr(CMDR, CMD_START | CMD_UPCLK | CMD_WAIT_PRE);
    bool ok = wait_clear(CMDR, CMD_START, 100000);
    wr(RINTR, 0xffffffffu);
    wr(CLKCR, c & ~CLKCR_MASK_D0);
    return ok ? ESP_OK : ESP_ERR_TIMEOUT;
}

static uint32_t pll_peri_1x(void)
{
    uint32_t pll = F101_REG32(PLL_PERI_REG);
    uint32_t n = ((pll >> 8) & 0xff) + 1, p0 = ((pll >> 16) & 7) + 1, m = (pll & 2) ? 2 : 1;

    return 24000000u / m / p0 * n / 2;
}

static esp_err_t set_clock_hz(uint32_t hz)
{
    uint32_t clkcr = rd(CLKCR) & ~(CLKCR_ON | CLKCR_LP);

    wr(CLKCR, clkcr);
    ESP_RETURN_ON_ERROR(update_clock(), TAG, "clock update failed");
    if (hz == 0) {
        return ESP_OK;
    }
    uint32_t target = hz * 2, src, mux;

    if (hz > 12000000u) {
        src = pll_peri_1x();
        mux = 1;
    } else {
        src = 24000000u;
        mux = 0;
    }
    uint32_t div = (src + target / 2) / target, p, m;
    if (div < 1) div = 1;
    if (div > 128) div = 128;
    p = div > 64 ? 3 : div > 32 ? 2 : div > 16 ? 1 : 0;
    m = (div + (1u << p) - 1) >> p;

    F101_REG32(SMHC_CLK_REG) = (mux << 24) | (p << 8) | (m - 1);
    wr(GCTRL, rd(GCTRL) & ~GCTRL_DDR);
    F101_REG32(SMHC_CLK_REG) = (1u << 31) | (mux << 24) | (p << 8) | (m - 1);
    wr(CLKCR, clkcr & ~0xffu);

    /* sample and drive phases follow the card clock */
    uint32_t drv = rd(DRV_DL) | (1u << 16);
    uint32_t ntsr = rd(NTSR) & ~((3u << 4) | (3u << 8));
    uint32_t sample = 0;
    if (hz > 26000000u && hz <= 52000000u) {
        drv |= (1u << 17);
        sample = 1;
    } else {
        drv &= ~(1u << 17);
    }
    wr(DRV_DL, drv);
    wr(NTSR, ntsr | (sample << 4) | (sample << 8) | (1u << 31));
    wr(CLKCR, (clkcr & ~0xffu) | CLKCR_ON);
    return update_clock();
}

static esp_err_t reset_ctrl(void)
{
    wr(GCTRL, rd(GCTRL) | GCTRL_ALL_RST | GCTRL_INT_EN | GCTRL_DONE_DIR);
    if (!wait_clear(GCTRL, GCTRL_ALL_RST, 100000)) {
        return ESP_ERR_TIMEOUT;
    }
    wr(RINTR, 0xffffffffu);
    wr(IMASK, 0);
    wr(TMOUT, (0xffffffu << 8) | 0xffu);
    return ESP_OK;
}

/* the hardware of a controller comes up in sdmmc_host_init_slot() */
esp_err_t sdmmc_host_init(void)
{
    ESP_RETURN_ON_FALSE(!s_host, ESP_ERR_INVALID_STATE, TAG, "already initialized");
    s_host = true;
    return ESP_OK;
}

static esp_err_t slot_hw_up(void)
{
    F101_REG32(SMHC_BGR) &= ~SMHC_RESET;
    F101_REG32(SMHC_BGR) |= SMHC_GATE;
    F101_REG32(SMHC_BGR) |= SMHC_RESET;
    F101_REG32(SMHC_CLK_REG) = (1u << 31);          /* module clock on (24 MHz): the controller reset needs it */
    ESP_RETURN_ON_ERROR(reset_ctrl(), TAG, "controller reset failed");
    wr(WIDTH, 0);
    s_width = 1;
    ESP_RETURN_ON_ERROR(set_clock_hz(400000), TAG, "clock setup failed");
    s_khz = 400;
    s_init = true;
    return ESP_OK;
}

/* The pads come from the slot configuration; the SoC says which ones can carry the signals. Only the
 * SMHC0 controller is a host here (slot 0). */
esp_err_t sdmmc_host_init_slot(int slot, const sdmmc_slot_config_t *cfg)
{
    static const f101_signal_t sig[6] = { F101_SIG_SMHC_CLK, F101_SIG_SMHC_CMD, F101_SIG_SMHC_D0, F101_SIG_SMHC_D1,
                                          F101_SIG_SMHC_D2, F101_SIG_SMHC_D3 };

    ESP_RETURN_ON_FALSE(s_host, ESP_ERR_INVALID_STATE, TAG, "host not initialized");
    ESP_RETURN_ON_FALSE(cfg, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_ERROR(enter(slot), TAG, "slot 0 (SMHC0) and slot 1 (SMHC2) are hosts");
    esp_err_t err = ESP_OK;

    if (cfg->width != 0 && cfg->width != 1 && cfg->width != 4) {
        err = ESP_ERR_INVALID_ARG;
        ESP_LOGE(TAG, "bus width is 1 or 4");
        goto out;
    }
    const gpio_num_t pins[6] = { cfg->clk, cfg->cmd, cfg->d0, cfg->d1, cfg->d2, cfg->d3 };
    int lines = (cfg->width == 1) ? 3 : 6;      /* clk, cmd, d0 or all */

    for (int i = 0; i < lines; i++) {
        if (pins[i] < 0 || f101_periph_pin_mux(sig[i], k_inst[slot], pins[i]) < 0) {
            ESP_LOGE(TAG, "gpio %d cannot carry sd signal %d of slot %d", (int)pins[i], i, slot);
            err = ESP_ERR_INVALID_ARG;
            goto out;
        }
    }
    for (int i = 0; i < lines; i++) {
        f101_periph_route(sig[i], k_inst[slot], pins[i]);
        f101_pin_pull(pins[i], i == 0 ? F101_PULL_NONE : F101_PULL_UP);
        f101_pin_drive(pins[i], 3);
    }
    s_slot_width = lines == 3 ? 1 : 4;
    err = slot_hw_up();
    s_slot_ready = err == ESP_OK;
out:
    leave();
    return err;
}

static esp_err_t do_set_bus_width(int slot, size_t width)
{
    ESP_RETURN_ON_FALSE(width == 1 || width == 4, ESP_ERR_INVALID_ARG, TAG, "unsupported bus width");
    ESP_RETURN_ON_FALSE(width <= s_slot_width, ESP_ERR_INVALID_ARG, TAG, "the slot has only %u data line(s) routed", (unsigned)s_slot_width);
    wr(WIDTH, width == 4 ? 1 : 0);
    s_width = width;
    return ESP_OK;
}

esp_err_t sdmmc_host_set_bus_width(int slot, size_t width)
{
    esp_err_t r;

    if (enter(slot) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    r = do_set_bus_width(slot, width);
    leave();
    return r;
}

static size_t do_get_slot_width(int slot)
{
    return s_width;
}

size_t sdmmc_host_get_slot_width(int slot)
{
    size_t r;

    if (enter(slot) != ESP_OK) {
        return 0;
    }
    r = do_get_slot_width(slot);
    leave();
    return r;
}

static esp_err_t do_set_card_clk(int slot, uint32_t freq_khz)
{
    ESP_RETURN_ON_ERROR(set_clock_hz(freq_khz * 1000), TAG, "clock setup failed");
    s_khz = freq_khz;
    return ESP_OK;
}

esp_err_t sdmmc_host_set_card_clk(int slot, uint32_t freq_khz)
{
    esp_err_t r;

    if (enter(slot) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    r = do_set_card_clk(slot, freq_khz);
    leave();
    return r;
}

static esp_err_t do_get_real_freq(int slot, int *real)
{
    *real = (int)s_khz;
    return ESP_OK;
}

esp_err_t sdmmc_host_get_real_freq(int slot, int *real)
{
    esp_err_t r;

    if (enter(slot) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    r = do_get_real_freq(slot, real);
    leave();
    return r;
}

esp_err_t sdmmc_host_set_bus_ddr_mode(int slot, bool en)
{
    return en ? ESP_ERR_NOT_SUPPORTED : ESP_OK;
}

esp_err_t sdmmc_host_set_cclk_always_on(int slot, bool on)
{
    return ESP_OK;
}

bool sdmmc_host_check_buffer_alignment(int slot, const void *buf, size_t size)
{
    return true;            /* the CPU moves the data, any alignment works */
}

esp_err_t sdmmc_host_deinit_slot(int slot)
{
    ESP_RETURN_ON_ERROR(enter(slot), TAG, "slot");
    if (s_intr[slot]) {
        esp_intr_free(s_intr[slot]);
        s_intr[slot] = NULL;
    }
    F101_REG32(SMHC_CLK_REG) = 0;
    F101_REG32(SMHC_BGR) &= ~(SMHC_RESET | SMHC_GATE);
    s_init = false;
    s_slot_ready = false;
    leave();
    return ESP_OK;
}

/* SDIO card interrupt (DAT1 low): masked in the interrupt, enabled again by io_int_wait() */
#define INT_SDIO        (1u << 16)

static void sdio_isr(void *arg)
{
    int c = (int)(intptr_t)arg;
    uint32_t base = k_base[c];
    BaseType_t woken = pdFALSE;
    uint32_t r = F101_REG32(base + RINTR);

    if (r & INT_SDIO) {
        F101_REG32(base + IMASK) &= ~INT_SDIO;
        F101_REG32(base + RINTR) = INT_SDIO;
        xSemaphoreGiveFromISR(s_sdio_sem[c], &woken);
    }
    if (woken) {
        portYIELD_FROM_ISR();
    }
}

esp_err_t sdmmc_host_io_int_enable(int slot)
{
    ESP_RETURN_ON_ERROR(enter(slot), TAG, "slot");
    esp_err_t err = ESP_OK;

    if (!s_init) {
        err = ESP_ERR_INVALID_STATE;
    } else if (!s_intr[slot]) {
        s_sdio_sem[slot] = s_sdio_sem[slot] ? s_sdio_sem[slot] : xSemaphoreCreateBinary();
        err = s_sdio_sem[slot] ? esp_intr_alloc(k_irq[slot], 0, sdio_isr, (void *)(intptr_t)slot, &s_intr[slot]) : ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK) {
        wr(RINTR, INT_SDIO);
        wr(IMASK, rd(IMASK) | INT_SDIO);
    }
    leave();
    return err;
}

esp_err_t sdmmc_host_io_int_wait(int slot, uint32_t timeout_ticks)
{
    if (slot < 0 || slot >= SLOTS || !s_sdio_sem[slot]) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_sdio_sem[slot], timeout_ticks) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    /* the card interrupt is serviced by the caller; unmask it for the next one */
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    g_c = slot;
    wr(IMASK, rd(IMASK) | INT_SDIO);
    xSemaphoreGiveRecursive(s_lock);
    return ESP_OK;
}

esp_err_t sdmmc_host_set_input_delay(int slot, sdmmc_delay_phase_t delay_phase)
{
    return delay_phase == SDMMC_DELAY_PHASE_0 ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
}

esp_err_t sdmmc_host_set_input_delayline(int slot, sdmmc_delay_line_t delay_line)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t sdmmc_host_is_slot_set_to_uhs1(int slot, bool *is_uhs1)
{
    *is_uhs1 = false;
    return ESP_OK;
}

esp_err_t sdmmc_host_deinit(void)
{
    for (int c = 0; c < SLOTS; c++) {
        if (s_init_a[c]) {
            sdmmc_host_deinit_slot(c);
        }
    }
    s_host = false;
    return ESP_OK;
}

static esp_err_t fifo_wait(uint32_t flag, uint64_t end)
{
    while (rd(STAS) & flag) {
        if (f101_mtime_get() > end) {
            return ESP_ERR_TIMEOUT;
        }
        if (rd(RINTR) & INT_ERR) {
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

static esp_err_t do_do_transaction(int slot, sdmmc_command_t *cmd)
{
    ESP_RETURN_ON_FALSE(s_init && cmd, ESP_ERR_INVALID_STATE, TAG, "host not ready");
    uint32_t cmdr = CMD_START | (cmd->opcode & 0x3f);
    uint32_t timeout_ms = cmd->timeout_ms ? cmd->timeout_ms : 1000;
    uint64_t end = f101_mtime_get() + (uint64_t)timeout_ms * (F101_MTIME_HZ / 1000);
    esp_err_t e = ESP_OK;
    bool data = cmd->data && cmd->datalen;
    bool write = data && !(cmd->flags & SCF_CMD_READ);
    uint32_t need = INT_CMD_DONE;

    if (cmd->flags & SCF_RSP_PRESENT) {
        cmdr |= CMD_RSP_EXP;
    }
    if (cmd->flags & SCF_RSP_136) {
        cmdr |= CMD_LONG;
    }
    if (cmd->flags & SCF_RSP_CRC) {
        cmdr |= CMD_CRC;
    }
    if (cmd->opcode == 0) {
        cmdr |= CMD_INIT;
    }

    /* wait for the card to stop signalling busy */
    {
        uint64_t bend = f101_mtime_get() + 2000ull * (F101_MTIME_HZ / 1000);
        while (rd(STAS) & STAS_BUSY) {
            if (f101_mtime_get() > bend) {
                return ESP_ERR_TIMEOUT;
            }
        }
    }
    wr(RINTR, 0xffffffffu);

    if (data) {
        uint32_t blk = cmd->blklen ? cmd->blklen : cmd->datalen;

        wr(BLKSZ, blk);
        wr(BCNTR, cmd->datalen);
        cmdr |= CMD_DATA | CMD_WAIT_PRE;
        if (write) {
            cmdr |= CMD_WRITE;
        }
        if (cmd->opcode == MMC_READ_BLOCK_MULTIPLE || cmd->opcode == MMC_WRITE_BLOCK_MULTIPLE) {
            cmdr |= CMD_AUTOSTOP;
            need = INT_AUTO_DONE;
        } else {
            need = INT_DATA_OVER;
        }
        wr(GCTRL, rd(GCTRL) | GCTRL_AHB | GCTRL_FIFO_RST);
        wait_clear(GCTRL, GCTRL_FIFO_RST, 100000);
    }
    wr(A12A, (cmdr & CMD_AUTOSTOP) ? 0 : 0xffff);
    wr(CARG, cmd->arg);
    wr(CMDR, cmdr);

    if (data) {
        uint8_t *buf = cmd->data;
        for (size_t i = 0; i < cmd->datalen && e == ESP_OK; i += 4) {
            uint32_t w = 0;
            size_t n = cmd->datalen - i < 4 ? cmd->datalen - i : 4;

            e = fifo_wait(write ? STAS_FULL : STAS_EMPTY, end);
            if (e != ESP_OK) {
                break;
            }
            if (write) {
                memcpy(&w, buf + i, n);
                wr(FIFO, w);
            } else {
                w = rd(FIFO);
                memcpy(buf + i, &w, n);
            }
        }
    }
    if (e == ESP_OK) {
        for (;;) {
            uint32_t r = rd(RINTR);

            if (r & INT_ERR) {
                e = (r & (INT_RESP_TO | INT_DATA_TO)) ? ESP_ERR_TIMEOUT : ESP_ERR_INVALID_CRC;
                break;
            }
            if ((r & need) == need) {
                break;
            }
            if (f101_mtime_get() > end) {
                e = ESP_ERR_TIMEOUT;
                break;
            }
        }
    }
    uint32_t rint = rd(RINTR);
    wr(RINTR, 0xffffffffu);
    if (data) {
        wr(GCTRL, rd(GCTRL) | GCTRL_FIFO_RST);
    }
    if (e == ESP_OK && (cmd->flags & SCF_RSP_PRESENT)) {
        for (int i = 0; i < ((cmd->flags & SCF_RSP_136) ? 4 : 1); i++) {
            cmd->response[i] = rd(RESP0 + 4 * i);
        }
    }
    if (e != ESP_OK) {
        (void)rint;
        /* leave the controller usable for the next command */
        uint32_t w = rd(WIDTH);
        reset_ctrl();
        wr(WIDTH, w);
        set_clock_hz(s_khz * 1000);
    } else if ((cmd->flags & SCF_RSP_BSY)) {
        uint64_t bend = f101_mtime_get() + 2000ull * (F101_MTIME_HZ / 1000);
        while (rd(STAS) & STAS_BUSY) {
            if (f101_mtime_get() > bend) {
                e = ESP_ERR_TIMEOUT;
                break;
            }
        }
    }
    cmd->error = e;
    return e;
}

esp_err_t sdmmc_host_do_transaction(int slot, sdmmc_command_t *cmd)
{
    esp_err_t r;

    if (enter(slot) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    r = do_do_transaction(slot, cmd);
    leave();
    return r;
}

/* the slot is fixed at 3.3 V: there is no voltage switching */
#include "sd_pwr_ctrl.h"

esp_err_t sd_pwr_ctrl_set_io_voltage(sd_pwr_ctrl_handle_t handle, int voltage_mv)
{
    (void)handle;
    (void)voltage_mv;
    return ESP_ERR_NOT_SUPPORTED;
}
