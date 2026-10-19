/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 */

/**
 * NOTE: this is a stripped-down copy to support building on host when using mocking (CMock).
 */

#pragma once

/* The pads of the console come from CONFIG_ESP_CONSOLE_UART_TX/RX_GPIO; the SoC has no fixed default pad. */
#define U0TXD_GPIO_NUM (-1)
#define U0RXD_GPIO_NUM (-1)
