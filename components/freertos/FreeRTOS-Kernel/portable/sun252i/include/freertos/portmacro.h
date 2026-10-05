/*
 * SPDX-FileCopyrightText: 2026 Yuzuki Tsuru
 * SPDX-License-Identifier: MIT
 *
 * FreeRTOS port of the XuanTie C907 (single core, machine mode, CLINT tick,
 * PLIC external interrupts).
 */
#ifndef PORTMACRO_H
#define PORTMACRO_H

#include "sdkconfig.h"
#include "freertos/FreeRTOSConfig.h"

#ifndef __ASSEMBLER__

#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <limits.h>
#include "spinlock.h"
#include "esp_macros.h"
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_rom_sys.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_newlib.h"

#ifdef __cplusplus
extern "C" {
#endif

#define portCHAR                    uint8_t
#define portFLOAT                   float
#define portDOUBLE                  double
#define portLONG                    int32_t
#define portSHORT                   int16_t
#define portSTACK_TYPE              uint8_t
#define portBASE_TYPE               int

typedef portSTACK_TYPE              StackType_t;
typedef portBASE_TYPE               BaseType_t;
typedef unsigned portBASE_TYPE      UBaseType_t;

#if (configUSE_16_BIT_TICKS == 1)
typedef uint16_t TickType_t;
#define portMAX_DELAY (TickType_t)  0xffff
#else
typedef uint32_t TickType_t;
#define portMAX_DELAY (TickType_t)  0xffffffffUL
#endif

#define portTASK_FUNCTION_PROTO(vFunction, pvParameters) void vFunction(void *pvParameters)
#define portTASK_FUNCTION(vFunction, pvParameters) void vFunction(void *pvParameters)

#define portCRITICAL_NESTING_IN_TCB     0
#define portSTACK_GROWTH                (-1)
#define portTICK_PERIOD_MS              ((TickType_t) (1000 / configTICK_RATE_HZ))
#define portBYTE_ALIGNMENT              16
#define portTICK_TYPE_IS_ATOMIC         1
#define portNOP() __asm volatile        (" nop ")

typedef spinlock_t                          portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED        SPINLOCK_INITIALIZER
#define portMUX_FREE_VAL                    SPINLOCK_FREE
#define portMUX_NO_TIMEOUT                  SPINLOCK_WAIT_FOREVER
#define portMUX_TRY_LOCK                    SPINLOCK_NO_WAIT
#define portMUX_INITIALIZE(mux)             spinlock_initialize(mux)

/* ----------------------- Interrupts -------------------------------------- */
UBaseType_t xPortSetInterruptMaskFromISR(void);
void vPortClearInterruptMaskFromISR(UBaseType_t prev_int_level);
BaseType_t xPortInIsrContext(void);
void vPortAssertIfInISR(void);
BaseType_t xPortInterruptedFromISRContext(void);

#define portSET_INTERRUPT_MASK_FROM_ISR()                   xPortSetInterruptMaskFromISR()
#define portCLEAR_INTERRUPT_MASK_FROM_ISR(prev_level)       vPortClearInterruptMaskFromISR(prev_level)
#define portDISABLE_INTERRUPTS()            ((void)xPortSetInterruptMaskFromISR())
#define portENABLE_INTERRUPTS()             vPortClearInterruptMaskFromISR(1)
#define portASSERT_IF_IN_ISR()              vPortAssertIfInISR()

/* ----------------------- Critical sections ------------------------------- */
void vPortEnterCritical(void);
void vPortExitCritical(void);

#define portENTER_CRITICAL(mux)                 {(void)mux;  vPortEnterCritical();}
#define portEXIT_CRITICAL(mux)                  {(void)mux;  vPortExitCritical();}
#define portTRY_ENTER_CRITICAL(mux, timeout)    ({  \
    (void)mux; (void)timeout;                       \
    vPortEnterCritical();                           \
    BaseType_t ret = pdPASS;                        \
    ret;                                            \
})
#define portENTER_CRITICAL_ISR(mux)                 portENTER_CRITICAL(mux)
#define portEXIT_CRITICAL_ISR(mux)                  portEXIT_CRITICAL(mux)
#define portTRY_ENTER_CRITICAL_ISR(mux, timeout)    portTRY_ENTER_CRITICAL(mux, timeout)
#define portENTER_CRITICAL_SAFE(mux)                portENTER_CRITICAL(mux)
#define portEXIT_CRITICAL_SAFE(mux)                 portEXIT_CRITICAL(mux)
#define portTRY_ENTER_CRITICAL_SAFE(mux, timeout)   portTRY_ENTER_CRITICAL(mux, timeout)

/* ----------------------- Yielding ---------------------------------------- */
void vPortYield(void);
void vPortYieldFromISR(void);

#define portYIELD() vPortYield()
#define portYIELD_FROM_ISR_NO_ARG() vPortYieldFromISR()
#define portYIELD_FROM_ISR_ARG(xHigherPriorityTaskWoken) ({ \
    if (xHigherPriorityTaskWoken == pdTRUE) { \
        vPortYieldFromISR(); \
    } \
})
#if defined(__cplusplus) && (__cplusplus >  201703L)
#define portYIELD_FROM_ISR(...) CHOOSE_MACRO_VA_ARG(portYIELD_FROM_ISR_ARG, portYIELD_FROM_ISR_NO_ARG __VA_OPT__(,) __VA_ARGS__)(__VA_ARGS__)
#else
#define portYIELD_FROM_ISR(...) CHOOSE_MACRO_VA_ARG(portYIELD_FROM_ISR_ARG, portYIELD_FROM_ISR_NO_ARG, ##__VA_ARGS__)(__VA_ARGS__)
#endif
#define portEND_SWITCHING_ISR(xSwitchRequired) if(xSwitchRequired) vPortYieldFromISR()
#define portYIELD_WITHIN_API() portYIELD()

FORCE_INLINE_ATTR bool xPortCanYield(void)
{
    return !xPortInIsrContext();
}

/* ----------------------- System ------------------------------------------ */
void vApplicationSleep(TickType_t xExpectedIdleTime);
#define portSUPPRESS_TICKS_AND_SLEEP(idleTime) vApplicationSleep(idleTime)

uint32_t xPortGetTickRateHz(void);
void vPortSetStackWatchpoint(void *pxStackStart);

FORCE_INLINE_ATTR BaseType_t xPortGetCoreID(void)
{
    return 0;
}
#define portGET_CORE_ID()       ((BaseType_t) 0)

#define portCONFIGURE_TIMER_FOR_RUN_TIME_STATS()
#if ( CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS )
configRUN_TIME_COUNTER_TYPE xPortGetRunTimeCounterValue( void );
#define portGET_RUN_TIME_COUNTER_VALUE()        xPortGetRunTimeCounterValue()
#endif

void vPortTCBPreDeleteHook( void *pxTCB );
#define portCLEAN_UP_TCB( pxTCB ) vPortTCBPreDeleteHook( pxTCB )

#if configUSE_PORT_OPTIMISED_TASK_SELECTION == 1
#define portRECORD_READY_PRIORITY( uxPriority, uxReadyPriorities ) ( uxReadyPriorities ) |= ( 1UL << ( uxPriority ) )
#define portRESET_READY_PRIORITY( uxPriority, uxReadyPriorities ) ( uxReadyPriorities ) &= ~( 1UL << ( uxPriority ) )
#define portGET_HIGHEST_PRIORITY( uxTopPriority, uxReadyPriorities ) uxTopPriority = ( 31 - __builtin_clz( ( uxReadyPriorities ) ) )
#endif

bool xPortCheckValidListMem(const void *ptr);
bool xPortCheckValidTCBMem(const void *ptr);
bool xPortcheckValidStackMem(const void *ptr);
#define portVALID_LIST_MEM(ptr)     xPortCheckValidListMem(ptr)
#define portVALID_TCB_MEM(ptr)      xPortCheckValidTCBMem(ptr)
#define portVALID_STACK_MEM(ptr)    xPortcheckValidStackMem(ptr)

#define os_task_switch_is_pended(_cpu_) (false)

#ifdef __cplusplus
}
#endif

#endif /* __ASSEMBLER__ */
#endif /* PORTMACRO_H */
