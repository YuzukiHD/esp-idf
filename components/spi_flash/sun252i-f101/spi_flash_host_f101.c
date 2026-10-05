/*
 * SPDX-FileCopyrightText: 2026 Yuzuki Tsuru
 * SPDX-License-Identifier: Apache-2.0
 *
 * SPI NOR flash on a SPI master bus: the host driver of the esp_flash layer
 * (spi_flash_host_driver_t) built on driver/spi_master.h, plus the default
 * chip (the EVB flash on SPI2_HOST, PC0..PC5, controller 0). Reads are single
 * wire (0x03), a transaction carries 8 bit command and 24 bit address.
 */

#include <string.h>
#include <stdlib.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_flash.h"
#include "esp_flash_spi_init.h"
#include "hal/spi_flash_hal.h"
#include "esp_flash_chips/spi_flash_chip_driver.h"
#include "esp_flash_chips/spi_flash_defs.h"
#include "esp_private/esp_flash_internal.h"
#include "esp_private/startup_internal.h"

static const char *TAG = "spi_flash";

#define FLASH_READ_MAX      4096u       /* bytes per read transaction */
#define FLASH_WRITE_SLICE   64u

esp_flash_t *esp_flash_default_chip;

typedef struct {
    spi_flash_hal_context_t ctx;        /* first: esp_flash treats the host as a memspi context */
    spi_host_device_t host_id;
    spi_device_handle_t dev;
    SemaphoreHandle_t lock;
} f101_flash_host_t;

static esp_err_t host_common_command(spi_flash_host_inst_t *host, spi_flash_trans_t *t)
{
    f101_flash_host_t *h = (f101_flash_host_t *)host;
    spi_transaction_t st = { .cmd = t->command, .addr = t->address };

    ESP_RETURN_ON_FALSE(t->address_bitlen == 0 || t->address_bitlen == 24, ESP_ERR_NOT_SUPPORTED, TAG, "24 bit addresses only");
    if (t->address_bitlen == 0) {
        st.flags |= SPI_TRANS_VARIABLE_ADDR;
    }
    if (t->mosi_len) {
        st.tx_buffer = t->mosi_data;
        st.length = (size_t)t->mosi_len * 8;
    }
    if (t->miso_len) {
        st.rx_buffer = t->miso_data;
        st.rxlength = (size_t)t->miso_len * 8;
    }
    return spi_device_polling_transmit(h->dev, &st);
}

static esp_err_t host_dev_config(spi_flash_host_inst_t *host)
{
    return ESP_OK;
}

static esp_err_t host_read_id(spi_flash_host_inst_t *host, uint32_t *id)
{
    uint32_t raw = 0;
    spi_flash_trans_t t = { .command = CMD_RDID, .miso_len = 3, .miso_data = (uint8_t *)&raw };

    ESP_RETURN_ON_ERROR(host_common_command(host, &t), TAG, "read id");
    if (raw == 0xffffff || raw == 0) {
        return ESP_ERR_FLASH_NO_RESPONSE;
    }
    *id = ((raw & 0xff) << 16) | (raw >> 16) | (raw & 0xff00);
    return ESP_OK;
}

static esp_err_t host_read_status(spi_flash_host_inst_t *host, uint8_t *out_sr)
{
    uint32_t sr = 0;
    spi_flash_trans_t t = { .command = CMD_RDSR, .miso_len = 1, .miso_data = (uint8_t *)&sr };
    esp_err_t err = host_common_command(host, &t);

    *out_sr = (uint8_t)sr;
    return err;
}

static void host_erase_chip(spi_flash_host_inst_t *host)
{
    spi_flash_trans_t t = { .command = CMD_CHIP_ERASE };
    host_common_command(host, &t);
}

static void host_erase_sector(spi_flash_host_inst_t *host, uint32_t addr)
{
    spi_flash_trans_t t = { .command = CMD_SECTOR_ERASE, .address_bitlen = 24, .address = addr };
    host_common_command(host, &t);
}

static void host_erase_block(spi_flash_host_inst_t *host, uint32_t addr)
{
    spi_flash_trans_t t = { .command = CMD_LARGE_BLOCK_ERASE, .address_bitlen = 24, .address = addr };
    host_common_command(host, &t);
}

static esp_err_t host_set_write_protect(spi_flash_host_inst_t *host, bool wp)
{
    spi_flash_trans_t t = { .command = wp ? CMD_WRDI : CMD_WREN };
    return host_common_command(host, &t);
}

/* the data length of spi_flash_trans_t is 8 bit: pages and reads longer than 255 bytes bypass it */
static void host_program_page(spi_flash_host_inst_t *host, const void *buf, uint32_t addr, uint32_t len)
{
    f101_flash_host_t *h = (f101_flash_host_t *)host;
    spi_transaction_t st = { .cmd = CMD_PROGRAM_PAGE, .addr = addr, .tx_buffer = buf, .length = (size_t)len * 8 };

    spi_device_polling_transmit(h->dev, &st);
}

static esp_err_t host_read(spi_flash_host_inst_t *host, void *buf, uint32_t addr, uint32_t len)
{
    f101_flash_host_t *h = (f101_flash_host_t *)host;
    spi_transaction_t st = { .cmd = CMD_READ, .addr = addr, .rx_buffer = buf, .rxlength = (size_t)len * 8 };

    return spi_device_polling_transmit(h->dev, &st);
}

static bool host_supports_direct(spi_flash_host_inst_t *host, const void *p)
{
    return true;                    /* no DMA or alignment restrictions */
}

static int host_write_slicer(spi_flash_host_inst_t *host, uint32_t address, uint32_t len, uint32_t *align_address, uint32_t page_size)
{
    /* the generic chip driver stages each slice in a 64 byte buffer: never longer than that, never across a page */
    uint32_t end_bound = (address / page_size + 1) * page_size;
    uint32_t max_len = end_bound - address;

    if (max_len > FLASH_WRITE_SLICE) {
        max_len = FLASH_WRITE_SLICE;
    }

    *align_address = address;
    return max_len < len ? max_len : len;
}

static int host_read_slicer(spi_flash_host_inst_t *host, uint32_t address, uint32_t len, uint32_t *align_address, uint32_t page_size)
{
    /* the generic chip driver stages each slice in a 64 byte stack buffer; chip_read() below goes around it */
    *align_address = address;
    return len < FLASH_WRITE_SLICE ? len : FLASH_WRITE_SLICE;
}

static uint32_t host_status(spi_flash_host_inst_t *host)
{
    return 1;                       /* non zero: the host is idle, every transfer is polled to completion */
}

static esp_err_t host_configure_io_mode(spi_flash_host_inst_t *host, uint32_t command, uint32_t addr_bitlen, int dummy_bitlen_base, esp_flash_io_mode_t io_mode)
{
    return io_mode == SPI_FLASH_SLOWRD ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
}

static void host_poll_cmd_done(spi_flash_host_inst_t *host)
{
}

static esp_err_t host_flush_cache(spi_flash_host_inst_t *host, uint32_t addr, uint32_t size)
{
    return ESP_OK;
}

static void host_check_suspend(spi_flash_host_inst_t *host)
{
}

static void host_resume(spi_flash_host_inst_t *host)
{
}

static void host_suspend(spi_flash_host_inst_t *host)
{
}

static esp_err_t host_sus_setup(spi_flash_host_inst_t *host, const spi_flash_sus_cmd_conf *conf)
{
    return ESP_ERR_NOT_SUPPORTED;
}

static const spi_flash_host_driver_t s_host_driver = {
    .dev_config = host_dev_config,
    .common_command = host_common_command,
    .read_id = host_read_id,
    .erase_chip = host_erase_chip,
    .erase_sector = host_erase_sector,
    .erase_block = host_erase_block,
    .read_status = host_read_status,
    .set_write_protect = host_set_write_protect,
    .program_page = host_program_page,
    .supports_direct_write = host_supports_direct,
    .write_data_slicer = host_write_slicer,
    .read = host_read,
    .supports_direct_read = host_supports_direct,
    .read_data_slicer = host_read_slicer,
    .host_status = host_status,
    .configure_host_io_mode = host_configure_io_mode,
    .poll_cmd_done = host_poll_cmd_done,
    .flush_cache = host_flush_cache,
    .check_suspend = host_check_suspend,
    .resume = host_resume,
    .suspend = host_suspend,
    .sus_setup = host_sus_setup,
};

/* ---- operating system hooks: one mutex per chip, delays through the scheduler ---- */

static esp_err_t os_start(void *arg, uint32_t flags)
{
    f101_flash_host_t *h = arg;

    xSemaphoreTake(h->lock, portMAX_DELAY);
    return ESP_OK;
}

static esp_err_t os_end(void *arg)
{
    f101_flash_host_t *h = arg;

    xSemaphoreGive(h->lock);
    return ESP_OK;
}

static esp_err_t os_region_protected(void *arg, size_t start, size_t size)
{
    return ESP_OK;
}

static esp_err_t os_delay_us(void *arg, uint32_t us)
{
    if (us >= 2 * portTICK_PERIOD_MS * 1000) {
        vTaskDelay(pdMS_TO_TICKS(us / 1000));
    } else {
        esp_rom_delay_us(us);
    }
    return ESP_OK;
}

static void *os_get_temp_buffer(void *arg, size_t req, size_t *out)
{
    *out = req;
    return heap_caps_malloc(req, MALLOC_CAP_DEFAULT);
}

static void os_release_temp_buffer(void *arg, void *buf)
{
    if (buf) {                      /* esp_flash calls this with NULL for reads that went directly to the caller buffer */
        free(buf);
    }
}

static esp_err_t os_check_yield(void *arg, uint32_t status, uint32_t *out_request)
{
    return ESP_ERR_TIMEOUT;         /* nothing to hand over: skip the yield */
}

static esp_err_t os_yield(void *arg, uint32_t *out_status)
{
    vTaskDelay(1);
    return ESP_OK;
}

static void os_set_flash_op_status(uint32_t status)
{
}

static const esp_flash_os_functions_t s_os_functions = {
    .start = os_start,
    .end = os_end,
    .region_protected = os_region_protected,
    .delay_us = os_delay_us,
    .get_temp_buffer = os_get_temp_buffer,
    .release_temp_buffer = os_release_temp_buffer,
    .check_yield = os_check_yield,
    .yield = os_yield,
    .set_flash_op_status = os_set_flash_op_status,
};

esp_err_t esp_flash_app_enable_os_functions(esp_flash_t *chip)
{
    chip->os_func = &s_os_functions;
    chip->os_func_data = (f101_flash_host_t *)chip->host;
    return ESP_OK;
}

esp_err_t esp_flash_app_disable_os_functions(esp_flash_t *chip)
{
    chip->os_func = NULL;
    return ESP_OK;
}

/* ---- chip driver: the detected driver with a direct read and a page wide write ---- */

static spi_flash_chip_t s_chip_drv;

static esp_err_t chip_read(esp_flash_t *chip, void *buffer, uint32_t address, uint32_t length)
{
    uint8_t *dst = buffer;
    esp_err_t err = ESP_OK;

    while (err == ESP_OK && length > 0) {
        uint32_t n = length < FLASH_READ_MAX ? length : FLASH_READ_MAX;

        err = chip->host->driver->read(chip->host, dst, address, n);
        address += n;
        dst += n;
        length -= n;
    }
    return err;
}

static esp_err_t chip_write(esp_flash_t *chip, const void *buffer, uint32_t address, uint32_t length)
{
    const uint8_t *src = buffer;
    const uint32_t page_size = chip->chip_drv->page_size;
    esp_err_t err = ESP_OK;

    while (err == ESP_OK && length > 0) {
        uint32_t n = page_size - (address % page_size);

        if (n > length) {
            n = length;
        }
        err = chip->chip_drv->set_chip_write_protect(chip, false);
        if (err == ESP_OK) {
            err = chip->chip_drv->program_page(chip, src, address, n);
        }
        address += n;
        src += n;
        length -= n;
    }
    return err;
}

static void patch_chip_driver(esp_flash_t *chip)
{
    s_chip_drv = *chip->chip_drv;
    s_chip_drv.read = chip_read;
    s_chip_drv.write = chip_write;
    chip->chip_drv = &s_chip_drv;
}

/* ---- bus and chip management ---- */

esp_err_t spi_bus_add_flash_device(esp_flash_t **out_chip, const esp_flash_spi_device_config_t *config)
{
    ESP_RETURN_ON_FALSE(out_chip && config, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(config->io_mode == SPI_FLASH_SLOWRD || config->io_mode == SPI_FLASH_FASTRD, ESP_ERR_NOT_SUPPORTED, TAG, "single wire reads only");

    f101_flash_host_t *h = calloc(1, sizeof(*h));
    esp_flash_t *chip = calloc(1, sizeof(*chip));
    esp_err_t err = ESP_ERR_NO_MEM;

    if (!h || !chip) {
        goto fail;
    }
    h->lock = xSemaphoreCreateMutex();
    h->host_id = config->host_id;
    h->ctx.inst.driver = &s_host_driver;
    if (!h->lock) {
        goto fail;
    }
    spi_device_interface_config_t dc = {
        .command_bits = 8,
        .address_bits = 24,
        .clock_speed_hz = (config->freq_mhz ? config->freq_mhz : 12) * 1000000,
        .mode = 0,
        .spics_io_num = config->cs_io_num,
        .queue_size = 1,
    };
    err = spi_bus_add_device(config->host_id, &dc, &h->dev);
    if (err != ESP_OK) {
        goto fail;
    }
    h->ctx.spi = h->dev;
    chip->host = &h->ctx.inst;
    chip->read_mode = SPI_FLASH_SLOWRD;
    chip->os_func = &s_os_functions;
    chip->os_func_data = h;
    *out_chip = chip;
    return ESP_OK;
fail:
    if (h && h->lock) {
        vSemaphoreDelete(h->lock);
    }
    free(h);
    free(chip);
    return err;
}

esp_err_t spi_bus_remove_flash_device(esp_flash_t *chip)
{
    ESP_RETURN_ON_FALSE(chip, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    f101_flash_host_t *h = (f101_flash_host_t *)chip->host;

    spi_bus_remove_device(h->dev);
    vSemaphoreDelete(h->lock);
    free(h);
    free(chip);
    return ESP_OK;
}

esp_err_t esp_flash_init_default_chip(void)
{
    /* the NOR flash of the board: SPI controller 0, pins PC0..PC5, chip select PC1 */
    spi_bus_config_t bc = {
        .sclk_io_num = GPIO_NUM_PC(0),
        .miso_io_num = GPIO_NUM_PC(2),
        .mosi_io_num = GPIO_NUM_PC(4),
        .quadwp_io_num = GPIO_NUM_PC(3),
        .quadhd_io_num = GPIO_NUM_PC(5),
        .max_transfer_sz = FLASH_READ_MAX,
    };
    esp_flash_spi_device_config_t fc = {
        .host_id = SPI2_HOST,
        .cs_io_num = GPIO_NUM_PC(1),
        .io_mode = SPI_FLASH_SLOWRD,
        .freq_mhz = 24,
    };
    esp_flash_t *chip;

    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &bc, SPI_DMA_DISABLED), TAG, "spi bus");
    ESP_RETURN_ON_ERROR(spi_bus_add_flash_device(&chip, &fc), TAG, "flash device");
    esp_err_t err = esp_flash_init(chip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "flash init failed: %s", esp_err_to_name(err));
        spi_bus_remove_flash_device(chip);
        return err;
    }
    patch_chip_driver(chip);
    esp_flash_default_chip = chip;
    return ESP_OK;
}

esp_err_t esp_flash_app_init(void)
{
    return ESP_OK;
}

esp_err_t esp_flash_app_init_os_functions(void)
{
    return ESP_OK;
}

ESP_SYSTEM_INIT_FN(init_flash, CORE, BIT(0), 130)
{
    esp_err_t err = esp_flash_init_default_chip();

    if (err == ESP_OK) {
        uint32_t size = 0;
        esp_flash_get_size(esp_flash_default_chip, &size);
        ESP_EARLY_LOGI(TAG, "flash id 0x%06x, %u MiB", (unsigned)esp_flash_default_chip->chip_id, (unsigned)(size >> 20));
    }
    return err;
}
