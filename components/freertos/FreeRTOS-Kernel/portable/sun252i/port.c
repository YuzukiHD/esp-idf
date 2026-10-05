/*
 * SPDX-FileCopyrightText: 2026 Yuzuki Tsuru
 * SPDX-License-Identifier: MIT
 */

#include <string.h>
#include "sdkconfig.h"
#include "soc/sun252i_f101_ll.h"
#include "FreeRTOS.h"
#include "task.h"
#include "portmacro.h"
#include "esp_attr.h"
#include "esp_system.h"
#include "esp_memory_utils.h"
#include "esp_log.h"
#include "esp_private/panic_internal.h"

#define MSTATUS_MPP_M       (3u << 11)

#define MIE_MSIE            (1u << 3)
#define MIE_MTIE            (1u << 7)
#define MIE_MEIE            (1u << 11)

#define TICK_CYCLES         (F101_MTIME_HZ / configTICK_RATE_HZ)

volatile uint32_t uxCriticalNesting;
volatile uint32_t port_outerMie;
volatile uint32_t port_uxInterruptNesting;
volatile uint32_t port_xYieldPending;
volatile uint32_t port_xSchedulerRunning;

uint8_t port_isr_stack[CONFIG_FREERTOS_ISR_STACKSIZE] __attribute__((aligned(16)));

extern void f101_trap_entry(void);
extern void vPortStartFirstTask(void);
extern void esp_intr_f101_dispatch(void);
extern void panicHandler(void *frame);

static void __attribute__((noreturn)) prvTaskExitError(void)
{
    abort();
}

extern int _thread_local_data_start, _thread_local_data_end;
extern int _thread_local_bss_start, _thread_local_bss_end;

/* the C library keeps errno and friends in thread local storage: each task
 * carries a copy at the top of its stack, tp points at it */
StackType_t *pxPortInitialiseStack(StackType_t *pxTopOfStack, TaskFunction_t pxCode, void *pvParameters)
{
    uint32_t top = ((uint32_t)pxTopOfStack) & ~0xfu;
    uint32_t tdata = (uint32_t)&_thread_local_data_end - (uint32_t)&_thread_local_data_start;
    uint32_t tbss = (uint32_t)&_thread_local_bss_end - (uint32_t)&_thread_local_bss_start;
    uint32_t area = (tdata + tbss + 15) & ~15u;
    uint32_t tls = (top - area) & ~0xfu;

    memcpy((void *)tls, &_thread_local_data_start, tdata);
    memset((void *)(tls + tdata), 0, tbss);

    uint32_t *frame = (uint32_t *)((tls - 176) & ~0xfu);

    memset(frame, 0, 176);
    frame[0] = (uint32_t)pxCode;                    /* mepc */
    frame[1] = (uint32_t)prvTaskExitError;          /* ra */
    frame[2] = (uint32_t)frame + 176;               /* sp */
    __asm volatile("mv %0, gp" : "=r"(frame[3]));
    frame[4] = tls;                                 /* tp */
    frame[10] = (uint32_t)pvParameters;             /* a0 */
    frame[32] = MSTATUS_MPIE | MSTATUS_MPP_M | (1u << 13);   /* mstatus: interrupts on after mret, FPU state initial (libgcc soft-float reads frm) */
    return (StackType_t *)frame;
}

/* ----------------------------- Timer -------------------------------------- */

static uint64_t s_next_tick;
static volatile uint64_t s_alarm = UINT64_MAX;
static void (*s_alarm_cb)(void *);
static void *s_alarm_arg;

static void prvProgramCompare(void)
{
    uint64_t next = s_next_tick < s_alarm ? s_next_tick : s_alarm;

    F101_REG32(F101_CLINT_MTIMECMP + 4) = 0xffffffffu;
    F101_REG32(F101_CLINT_MTIMECMP) = (uint32_t)next;
    F101_REG32(F101_CLINT_MTIMECMP + 4) = (uint32_t)(next >> 32);
}

void f101_timer_set_alarm_mtime(uint64_t mtime_deadline)
{
    s_alarm = mtime_deadline;
    prvProgramCompare();
}

void f101_timer_set_alarm_cb(void (*cb)(void *), void *arg)
{
    s_alarm_arg = arg;
    s_alarm_cb = cb;
}

static void prvSetNextTick(void)
{
    s_next_tick = f101_mtime_get() + TICK_CYCLES;
    prvProgramCompare();
}

static void prvTimerInterrupt(void)
{
    uint64_t now = f101_mtime_get();

    if (now >= s_next_tick) {
        s_next_tick += TICK_CYCLES;
        if (s_next_tick <= now) {
            s_next_tick = now + TICK_CYCLES;
        }
        if (xTaskIncrementTick() != pdFALSE) {
            port_xYieldPending = 1;
        }
    }
    if (now >= s_alarm) {
        s_alarm = UINT64_MAX;
        if (s_alarm_cb) {
            s_alarm_cb(s_alarm_arg);
        }
    }
    prvProgramCompare();
}

/* ----------------------------- Traps -------------------------------------- */

void f101_trap_handler(uint32_t mcause, uint32_t mepc, uint32_t *frame)
{
    (void)mepc;

    if (mcause & 0x80000000u) {
        switch (mcause & 0x1f) {
        case 7:     /* machine timer: the tick */
            prvTimerInterrupt();
            break;
        case 11:    /* machine external: PLIC */
            esp_intr_f101_dispatch();
            break;
        case 3:     /* machine software */
            F101_REG32(F101_CLINT_BASE) = 0;
            port_xYieldPending = 1;
            break;
        default:
            break;
        }
        return;
    }

    if (mcause == 11 || mcause == 8) {  /* ecall: a task asks for a switch */
        frame[0] += 4;
        port_xYieldPending = 1;
        return;
    }

    panicHandler(frame);
}

/* ----------------------------- Scheduler ---------------------------------- */

BaseType_t xPortStartScheduler(void)
{
    __asm volatile("csrw mtvec, %0" :: "r"((uint32_t)f101_trap_entry));
    uxCriticalNesting = 0;
    port_uxInterruptNesting = 0;

    prvSetNextTick();
    __asm volatile("csrs mie, %0" :: "r"(MIE_MTIE | MIE_MEIE | MIE_MSIE));
    vPortStartFirstTask();
    return pdFALSE;
}

void vPortEndScheduler(void)
{
    abort();
}

/* ----------------------------- Context ------------------------------------ */

BaseType_t xPortInIsrContext(void)
{
    return port_uxInterruptNesting != 0;
}

BaseType_t xPortInterruptedFromISRContext(void)
{
    return port_uxInterruptNesting > 1;
}

void vPortAssertIfInISR(void)
{
    configASSERT(port_uxInterruptNesting == 0);
}

UBaseType_t xPortSetInterruptMaskFromISR(void)
{
    uint32_t prev;

    __asm volatile("csrrci %0, mstatus, %1" : "=r"(prev) : "i"(MSTATUS_MIE));
    return (prev & MSTATUS_MIE) ? 1 : 0;
}

void vPortClearInterruptMaskFromISR(UBaseType_t prev)
{
    if (prev) {
        __asm volatile("csrsi mstatus, %0" :: "i"(MSTATUS_MIE));
    }
}

void vPortEnterCritical(void)
{
    uint32_t prev;

    __asm volatile("csrrci %0, mstatus, %1" : "=r"(prev) : "i"(MSTATUS_MIE));
    if (uxCriticalNesting == 0) {
        port_outerMie = (prev & MSTATUS_MIE) ? 1 : 0;
    }
    uxCriticalNesting++;
}

void vPortExitCritical(void)
{
    configASSERT(uxCriticalNesting > 0);
    uxCriticalNesting--;
    if (uxCriticalNesting == 0 && port_outerMie) {
        __asm volatile("csrsi mstatus, %0" :: "i"(MSTATUS_MIE));
    }
}

void vPortYieldFromISR(void)
{
    port_xYieldPending = 1;
}

/* ----------------------------- Hooks -------------------------------------- */

void vApplicationSleep(TickType_t xExpectedIdleTime)
{
    (void)xExpectedIdleTime;
}

uint32_t xPortGetTickRateHz(void)
{
    return configTICK_RATE_HZ;
}

void vPortSetStackWatchpoint(void *pxStackStart)
{
    (void)pxStackStart;
}

void vPortTCBPreDeleteHook(void *pxTCB)
{
    (void)pxTCB;
}

#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
configRUN_TIME_COUNTER_TYPE xPortGetRunTimeCounterValue(void)
{
    return (configRUN_TIME_COUNTER_TYPE)(f101_mtime_get() / (F101_MTIME_HZ / 1000000u));
}
#endif

void __attribute__((weak)) vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    esp_rom_printf("\n***ERROR*** A stack overflow in task %s has been detected.\n", pcTaskName);
    abort();
}
