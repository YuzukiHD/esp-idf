/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

/* no shared SPI bus arbitration (the bus lock of the ESP SPI master): only the handle type that esp_flash_internal.h names */
typedef struct spi_bus_lock_dev_t *spi_bus_lock_dev_handle_t;
