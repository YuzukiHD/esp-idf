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
#include "f101_sdmmc.h"
#include "soc/sun252i_f101_ll.h"
#include "freertos/FreeRTOS.h"

#define SMHC_BASE       0x04020000u
#define SMHC_BGR        (F101_CCU_BASE + 0x84cu)
#define SMHC_CLK_REG    (F101_CCU_BASE + 0x830u)
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

static bool s_init;
static size_t s_width = 1;
static uint32_t s_khz;

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

static esp_err_t host_init(void)
{
    ESP_RETURN_ON_FALSE(!s_init, ESP_ERR_INVALID_STATE, TAG, "already initialized");
    F101_REG32(SMHC_BGR) &= ~(1u << 16);
    F101_REG32(SMHC_BGR) |= 1u;
    F101_REG32(SMHC_BGR) |= (1u << 16);
    F101_REG32(SMHC_CLK_REG) = (1u << 31);          /* module clock on (24 MHz): the controller reset needs it */
    /* PF0..PF5 carry the SD slot: clock, command and four data lines at mux 2 */
    for (int n = 0; n < 6; n++) {
        int pin = 5 * 32 + n;
        f101_pin_mux(pin, 2);
        f101_pin_pull(pin, F101_PULL_UP);
        f101_pin_drive(pin, 3);
    }
    ESP_RETURN_ON_ERROR(reset_ctrl(), TAG, "controller reset failed");
    wr(WIDTH, 0);
    s_width = 1;
    ESP_RETURN_ON_ERROR(set_clock_hz(400000), TAG, "clock setup failed");
    s_khz = 400;
    s_init = true;
    return ESP_OK;
}

static esp_err_t host_set_bus_width(int slot, size_t width)
{
    ESP_RETURN_ON_FALSE(width == 1 || width == 4, ESP_ERR_INVALID_ARG, TAG, "unsupported bus width");
    wr(WIDTH, width == 4 ? 1 : 0);
    s_width = width;
    return ESP_OK;
}

static size_t host_get_bus_width(int slot)
{
    return s_width;
}

static esp_err_t host_set_card_clk(int slot, uint32_t freq_khz)
{
    ESP_RETURN_ON_ERROR(set_clock_hz(freq_khz * 1000), TAG, "clock setup failed");
    s_khz = freq_khz;
    return ESP_OK;
}

static esp_err_t host_get_real_freq(int slot, int *real)
{
    *real = (int)s_khz;
    return ESP_OK;
}

static esp_err_t host_set_ddr(int slot, bool en)
{
    return en ? ESP_ERR_NOT_SUPPORTED : ESP_OK;
}

static esp_err_t host_set_always_on(int slot, bool on)
{
    return ESP_OK;
}

static bool host_check_buffer_alignment(int slot, const void *buf, size_t size)
{
    return true;            /* the CPU moves the data, any alignment works */
}

static esp_err_t host_deinit(void)
{
    F101_REG32(SMHC_CLK_REG) = 0;
    F101_REG32(SMHC_BGR) &= ~((1u << 16) | 1u);
    s_init = false;
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

static esp_err_t host_do_transaction(int slot, sdmmc_command_t *cmd)
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

sdmmc_host_t f101_sdmmc_host(void)
{
    sdmmc_host_t h = {
        .flags = SDMMC_HOST_FLAG_1BIT | SDMMC_HOST_FLAG_4BIT,
        .slot = 0,
        .max_freq_khz = SDMMC_FREQ_HIGHSPEED,
        .io_voltage = 3.3f,
        .init = &host_init,
        .set_bus_width = &host_set_bus_width,
        .get_bus_width = &host_get_bus_width,
        .set_bus_ddr_mode = &host_set_ddr,
        .set_card_clk = &host_set_card_clk,
        .set_cclk_always_on = &host_set_always_on,
        .do_transaction = &host_do_transaction,
        .deinit = &host_deinit,
        .io_int_enable = NULL,
        .io_int_wait = NULL,
        .command_timeout_ms = 0,
        .get_real_freq = &host_get_real_freq,
        .check_buffer_alignment = &host_check_buffer_alignment,
        .input_delay_phase = SDMMC_DELAY_PHASE_0,
    };
    return h;
}

/* the slot is fixed at 3.3 V: there is no voltage switching */
#include "sd_pwr_ctrl.h"

esp_err_t sd_pwr_ctrl_set_io_voltage(sd_pwr_ctrl_handle_t handle, int voltage_mv)
{
    (void)handle;
    (void)voltage_mv;
    return ESP_ERR_NOT_SUPPORTED;
}
