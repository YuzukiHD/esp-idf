/*
 * SPDX-FileCopyrightText: 2024-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sdkconfig.h"
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio-bufio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <signal.h>
#include <sys/unistd.h>
#include <sys/reent.h>
#include <assert.h>
#include <fcntl.h>
#include "esp_newlib.h"
#include "esp_attr.h"
#include "soc/soc_caps.h"
#include "esp_rom_caps.h"
#include "esp_heap_caps.h"

/* TODO IDF-11226 */
void esp_newlib_init(void) __attribute__((alias("esp_libc_init")));
void esp_libc_init(void)
{
    environ = malloc(sizeof(char*));
    if (environ == 0) {
        // if allocation fails this early in startup process, there's nothing else other than to panic.
        abort();
    }
    environ[0] = NULL;

    esp_libc_locks_init();
}

void esp_setup_newlib_syscalls(void) __attribute__((alias("esp_libc_init")));

/* dummy function for backward compatibility */
void esp_reent_cleanup(void)
{
    return;
}

/*
 * Initialize stdin, stdout, and stderr using static memory allocation.
 * Creating them with fopen() would call malloc() internally.
 */
static char in_buf[BUFSIZ];
static char out_buf[BUFSIZ];
static char err_buf[BUFSIZ];

static struct __file_bufio __stdin = FDEV_SETUP_BUFIO(0, in_buf, BUFSIZ, read, write, lseek, close, __SRD, 0);
static struct __file_bufio __stdout = FDEV_SETUP_BUFIO(1, out_buf, BUFSIZ, read, write, lseek, close, __SWR, __BLBF);
static struct __file_bufio __stderr = FDEV_SETUP_BUFIO(2, err_buf, BUFSIZ, read, write, lseek, close, __SWR, __BLBF);

FILE *stdin = &__stdin.xfile.cfile.file;
FILE *stdout = &__stdout.xfile.cfile.file;
FILE *stderr = &__stderr.xfile.cfile.file;

#if CONFIG_LIBC_PICOLIBC_NEWLIB_COMPATIBILITY
__thread FILE* tls_stdin = &__stdin.xfile.cfile.file;
__thread FILE* tls_stdout = &__stdout.xfile.cfile.file;
__thread FILE* tls_stderr = &__stderr.xfile.cfile.file;
#endif

#if CONFIG_VFS_SUPPORT_IO
void esp_libc_init_global_stdio(const char *stdio_dev)
{
    int stdin_fd = open(stdio_dev, O_RDONLY);
    assert(stdin_fd >= 0); // Should be 0
    __stdin.ptr = (void *)(intptr_t)(stdin_fd);

    int stdout_fd = open(stdio_dev, O_WRONLY);
    assert(stdout_fd >= 0); // Should be 1
    __stdout.ptr = (void *)(intptr_t)(stdout_fd);

    int stderr_fd = open(stdio_dev, O_WRONLY);
    assert(stderr_fd >= 0); // Should be 2
    __stderr.ptr = (void *)(intptr_t)(stderr_fd);
}
#else  /* CONFIG_VFS_SUPPORT_IO */
void esp_libc_init_global_stdio(void)
{
    /* Nothing to do. */
}
#endif  /* CONFIG_VFS_SUPPORT_IO */

// Hook to force the linker to include this file
void esp_libc_include_init_funcs(void)
{
}
