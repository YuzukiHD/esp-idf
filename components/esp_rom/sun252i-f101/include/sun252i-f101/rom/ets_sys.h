/*
 * SPDX-FileCopyrightText: 2026 Yuzuki Tsuru
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ETSTimerFunc)(void *timer_arg);

typedef struct _ETSTIMER_ {
    struct _ETSTIMER_ *timer_next;
    uint32_t timer_expire;
    uint32_t timer_period;
    ETSTimerFunc timer_func;
    void *timer_arg;
} ETSTimer;

void ets_install_putc1(void (*p)(char c));
void ets_install_putc2(void (*p)(char c));

#ifdef __cplusplus
}
#endif
