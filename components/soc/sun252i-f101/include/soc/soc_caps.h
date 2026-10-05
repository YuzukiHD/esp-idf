/*
 * SPDX-FileCopyrightText: 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * These defines are parsed and imported as kconfig variables via the script
 * `tools/gen_soc_caps_kconfig/gen_soc_caps_kconfig.py`
 */

#pragma once

#if __has_include("soc/soc_caps_eval.h")
#include "soc/soc_caps_eval.h"
#endif

/*-------------------------- COMMON CAPS ---------------------------------------*/
#define SOC_GPIO_SUPPORTED              (1)
#define SOC_UART_SUPPORTED              (1)
#define SOC_I2C_SUPPORTED               (1)

/*-------------------------- CPU CAPS ----------------------------------------*/
#define SOC_CPU_HAS_FPU                 (1)
#define SOC_CPU_CORES_NUM               (1U)
#define SOC_CPU_INTR_NUM                (32)
#define SOC_CPU_WATCHPOINTS_NUM         (4)

/*-------------------------- GPIO CAPS ---------------------------------------*/
#define SOC_GPIO_PORT                   (6U)           /* PA..PF */
#define SOC_GPIO_PIN_COUNT              (192)
#define SOC_GPIO_IN_RANGE_MAX           (191)
#define SOC_GPIO_OUT_RANGE_MAX          (191)

/*-------------------------- UART CAPS ---------------------------------------*/
#define SOC_UART_NUM                    (6)
#define SOC_UART_HP_NUM                 (6)
#define SOC_UART_FIFO_LEN               (128)

/*-------------------------- I2C CAPS ----------------------------------------*/
#define SOC_I2C_NUM                     (2)
#define SOC_I2C_SUPPORT_SLAVE           (0)
#define SOC_UART_WAKEUP_SUPPORT_FIFO_THRESH_MODE (1)
#define SOC_CPU_BREAKPOINTS_NUM         (2)

/*-------------------------- SPI CAPS ----------------------------------------*/
#define SOC_SPI_PERIPH_NUM              (3)     /* SPI1 host unused, SPI2 = controller 0, SPI3 = controller 1 */
#define SOC_SPI_MAX_CS_NUM              (4)
#define SOC_SPI_MAXIMUM_BUFFER_SIZE     (64)
#define SOC_SPI_SUPPORT_MASTER_HD_VERSION_2 (0)

/*-------------------------- LEDC (PWM) CAPS ---------------------------------*/
/*-------------------------- I2S CAPS ----------------------------------------*/
#define SOC_I2S_SUPPORTED               (1)
#define SOC_I2S_NUM                     (1U)        /* the on-chip audio codec is I2S_NUM_0 */
#define SOC_I2S_HW_VERSION_2            (1)
#define SOC_I2S_SUPPORTS_PLL_F160M      (0)

#define SOC_LCD_RGB_SUPPORTED           (1)
#define SOC_FLASH_ENCRYPTED_XTS_AES_BLOCK_MAX (64)
#define SOC_LCDCAM_RGB_LCD_SUPPORTED    (1)
#define SOC_LEDC_SUPPORTED              (1)
#define SOC_LEDC_TIMER_NUM              (4)
#define SOC_LEDC_CHANNEL_NUM            (4)
#define SOC_LEDC_TIMER_BIT_WIDTH        (16)
#define SOC_LEDC_SUPPORT_APB_CLOCK      (1)
