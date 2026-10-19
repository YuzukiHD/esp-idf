/*
 * SPDX-FileCopyrightText: 2026 Yuzuki Tsuru
 * SPDX-License-Identifier: Apache-2.0
 *
 * The console file system of the UARTs: the descriptor is the port number, writes go to the FIFO with
 * LF turned into CR LF, reads wait on the receive buffer of the driver (the driver has to be installed).
 */

#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "sdkconfig.h"
#include "esp_vfs.h"
#include "driver/uart.h"
#include "driver/esp_private/uart_vfs.h"
#include "soc/sun252i_f101_ll.h"

static int vfs_open(void *ctx, const char *path, int flags, int mode)
{
    int n = (path && path[0] == '/') ? path[1] - '0' : -1;

    if (n < 0 || n >= SOC_UART_HP_NUM || path[2] != '\0') {
        errno = ENOENT;
        return -1;
    }
    return n;
}

static ssize_t vfs_write(void *ctx, int fd, const void *data, size_t size)
{
    const char *p = data;

    for (size_t i = 0; i < size; i++) {
        if (p[i] == '\n') {
            f101_uart_putc(fd, '\r');
        }
        f101_uart_putc(fd, p[i]);
    }
    return (ssize_t)size;
}

static ssize_t vfs_read(void *ctx, int fd, void *data, size_t size)
{
    if (!uart_is_driver_installed(fd)) {
        errno = ENODEV;
        return -1;
    }
    int n = uart_read_bytes(fd, data, size > 0 ? 1 : 0, portMAX_DELAY);

    return n < 0 ? -1 : n;
}

static int vfs_fstat(void *ctx, int fd, struct stat *st)
{
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFCHR;
    return 0;
}

static int vfs_close(void *ctx, int fd)
{
    return 0;
}

static int vfs_fsync(void *ctx, int fd)
{
    return uart_wait_tx_done(fd, portMAX_DELAY) == ESP_OK ? 0 : -1;
}

static const esp_vfs_fs_ops_t s_vfs_uart = {
    .write_p = vfs_write,
    .open_p = vfs_open,
    .fstat_p = vfs_fstat,
    .close_p = vfs_close,
    .read_p = vfs_read,
    .fsync_p = vfs_fsync,
};

const esp_vfs_fs_ops_t *esp_vfs_uart_get_vfs(void)
{
    return &s_vfs_uart;
}
