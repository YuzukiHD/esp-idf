/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

/* peripheral clock/reset gating is done by the drivers through the CCU */
typedef enum {
    PERIPH_UART0_MODULE,
    PERIPH_UART1_MODULE,
    PERIPH_UART2_MODULE,
    PERIPH_UART3_MODULE,
    PERIPH_MODULE_MAX
} shared_periph_module_t;
