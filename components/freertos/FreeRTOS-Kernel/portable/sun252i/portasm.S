/*
 * SPDX-FileCopyrightText: 2026 Yuzuki Tsuru
 * SPDX-License-Identifier: MIT
 *
 * Trap entry, context switch and first task start. The frame is RvExcFrame
 * compatible (esp_system panic code reads it), followed by two port words and
 * the single precision FPU state: every trap saves f0..f31 and fcsr, so the C
 * code of interrupt handlers and of the scheduler may use the FPU freely and
 * each task keeps its own FPU registers and rounding mode.
 */
#include "sdkconfig.h"
#include "freertos/FreeRTOSConfig.h"

#define FRAME_SIZE      320     /* 176 byte RvExcFrame + port words, then f0..f31 and fcsr */
#define F_FP            176     /* f0..f31, 4 bytes each */
#define F_FCSR          304
#define F_MEPC          0
#define F_RA            4
#define F_SP            8
#define F_GP            12
#define F_TP            16
#define F_T0            20
#define F_T1            24
#define F_T2            28
#define F_S0            32
#define F_S1            36
#define F_A0            40
#define F_A1            44
#define F_A2            48
#define F_A3            52
#define F_A4            56
#define F_A5            60
#define F_A6            64
#define F_A7            68
#define F_S2            72
#define F_S3            76
#define F_S4            80
#define F_S5            84
#define F_S6            88
#define F_S7            92
#define F_S8            96
#define F_S9            100
#define F_S10           104
#define F_S11           108
#define F_T3            112
#define F_T4            116
#define F_T5            120
#define F_T6            124
#define F_MSTATUS       128
#define F_MTVEC         132
#define F_MCAUSE        136
#define F_MTVAL         140
#define F_MHARTID       144
#define F_CRIT          148     /* port: uxCriticalNesting */
#define F_OUTER         152     /* port: saved MIE of the outermost critical section */

    .section .text
    .balign 4
    .global f101_trap_entry
    .type   f101_trap_entry, @function
f101_trap_entry:
    addi    sp, sp, -FRAME_SIZE
    sw      ra, F_RA(sp)
    sw      gp, F_GP(sp)
    sw      tp, F_TP(sp)
    sw      t0, F_T0(sp)
    sw      t1, F_T1(sp)
    sw      t2, F_T2(sp)
    sw      s0, F_S0(sp)
    sw      s1, F_S1(sp)
    sw      a0, F_A0(sp)
    sw      a1, F_A1(sp)
    sw      a2, F_A2(sp)
    sw      a3, F_A3(sp)
    sw      a4, F_A4(sp)
    sw      a5, F_A5(sp)
    sw      a6, F_A6(sp)
    sw      a7, F_A7(sp)
    sw      s2, F_S2(sp)
    sw      s3, F_S3(sp)
    sw      s4, F_S4(sp)
    sw      s5, F_S5(sp)
    sw      s6, F_S6(sp)
    sw      s7, F_S7(sp)
    sw      s8, F_S8(sp)
    sw      s9, F_S9(sp)
    sw      s10, F_S10(sp)
    sw      s11, F_S11(sp)
    sw      t3, F_T3(sp)
    sw      t4, F_T4(sp)
    sw      t5, F_T5(sp)
    sw      t6, F_T6(sp)
    addi    t0, sp, FRAME_SIZE
    sw      t0, F_SP(sp)
    csrr    t0, mepc
    sw      t0, F_MEPC(sp)
    csrr    t0, mstatus
    sw      t0, F_MSTATUS(sp)
    csrr    t0, mtvec
    sw      t0, F_MTVEC(sp)
    csrr    t0, mcause
    sw      t0, F_MCAUSE(sp)
    csrr    t0, mtval
    sw      t0, F_MTVAL(sp)
    sw      zero, F_MHARTID(sp)
    la      t0, uxCriticalNesting
    lw      t1, 0(t0)
    sw      t1, F_CRIT(sp)
    la      t0, port_outerMie
    lw      t1, 0(t0)
    sw      t1, F_OUTER(sp)

    frcsr   t0
    sw      t0, F_FCSR(sp)
    fsw     f0, F_FP+0(sp)
    fsw     f1, F_FP+4(sp)
    fsw     f2, F_FP+8(sp)
    fsw     f3, F_FP+12(sp)
    fsw     f4, F_FP+16(sp)
    fsw     f5, F_FP+20(sp)
    fsw     f6, F_FP+24(sp)
    fsw     f7, F_FP+28(sp)
    fsw     f8, F_FP+32(sp)
    fsw     f9, F_FP+36(sp)
    fsw     f10, F_FP+40(sp)
    fsw     f11, F_FP+44(sp)
    fsw     f12, F_FP+48(sp)
    fsw     f13, F_FP+52(sp)
    fsw     f14, F_FP+56(sp)
    fsw     f15, F_FP+60(sp)
    fsw     f16, F_FP+64(sp)
    fsw     f17, F_FP+68(sp)
    fsw     f18, F_FP+72(sp)
    fsw     f19, F_FP+76(sp)
    fsw     f20, F_FP+80(sp)
    fsw     f21, F_FP+84(sp)
    fsw     f22, F_FP+88(sp)
    fsw     f23, F_FP+92(sp)
    fsw     f24, F_FP+96(sp)
    fsw     f25, F_FP+100(sp)
    fsw     f26, F_FP+104(sp)
    fsw     f27, F_FP+108(sp)
    fsw     f28, F_FP+112(sp)
    fsw     f29, F_FP+116(sp)
    fsw     f30, F_FP+120(sp)
    fsw     f31, F_FP+124(sp)

    mv      s0, sp                       /* frame, survives the C call */
    la      t0, port_uxInterruptNesting
    lw      t1, 0(t0)
    addi    t2, t1, 1
    sw      t2, 0(t0)
    bnez    t1, 1f                       /* nested: stay on this stack */
    la      t0, port_xSchedulerRunning
    lw      t0, 0(t0)
    beqz    t0, 0f
    la      t0, pxCurrentTCBs
    lw      t0, 0(t0)
    sw      sp, 0(t0)                    /* pxTopOfStack = frame */
0:  la      t0, port_isr_stack
    li      t1, CONFIG_FREERTOS_ISR_STACKSIZE
    add     sp, t0, t1
1:
    csrr    a0, mcause
    csrr    a1, mepc
    mv      a2, s0
    call    f101_trap_handler

    la      t0, port_uxInterruptNesting
    lw      t1, 0(t0)
    addi    t1, t1, -1
    sw      t1, 0(t0)
    bnez    t1, 5f                       /* nested: back to the interrupted frame */
    la      t0, port_xSchedulerRunning
    lw      t0, 0(t0)
    beqz    t0, 5f
    la      t0, port_xYieldPending
    lw      t1, 0(t0)
    beqz    t1, 4f
    sw      zero, 0(t0)
    call    vTaskSwitchContext
4:  la      t0, pxCurrentTCBs
    lw      t0, 0(t0)
    lw      sp, 0(t0)                    /* frame of the task to run */
    j       6f
5:  mv      sp, s0
6:
    lw      t0, F_FCSR(sp)
    csrw    fcsr, t0
    flw     f0, F_FP+0(sp)
    flw     f1, F_FP+4(sp)
    flw     f2, F_FP+8(sp)
    flw     f3, F_FP+12(sp)
    flw     f4, F_FP+16(sp)
    flw     f5, F_FP+20(sp)
    flw     f6, F_FP+24(sp)
    flw     f7, F_FP+28(sp)
    flw     f8, F_FP+32(sp)
    flw     f9, F_FP+36(sp)
    flw     f10, F_FP+40(sp)
    flw     f11, F_FP+44(sp)
    flw     f12, F_FP+48(sp)
    flw     f13, F_FP+52(sp)
    flw     f14, F_FP+56(sp)
    flw     f15, F_FP+60(sp)
    flw     f16, F_FP+64(sp)
    flw     f17, F_FP+68(sp)
    flw     f18, F_FP+72(sp)
    flw     f19, F_FP+76(sp)
    flw     f20, F_FP+80(sp)
    flw     f21, F_FP+84(sp)
    flw     f22, F_FP+88(sp)
    flw     f23, F_FP+92(sp)
    flw     f24, F_FP+96(sp)
    flw     f25, F_FP+100(sp)
    flw     f26, F_FP+104(sp)
    flw     f27, F_FP+108(sp)
    flw     f28, F_FP+112(sp)
    flw     f29, F_FP+116(sp)
    flw     f30, F_FP+120(sp)
    flw     f31, F_FP+124(sp)
    lw      t0, F_MEPC(sp)
    csrw    mepc, t0
    lw      t0, F_MSTATUS(sp)
    csrw    mstatus, t0
    lw      t0, F_CRIT(sp)
    la      t1, uxCriticalNesting
    sw      t0, 0(t1)
    lw      t0, F_OUTER(sp)
    la      t1, port_outerMie
    sw      t0, 0(t1)
    lw      ra, F_RA(sp)
    lw      gp, F_GP(sp)
    lw      tp, F_TP(sp)
    lw      t0, F_T0(sp)
    lw      t1, F_T1(sp)
    lw      t2, F_T2(sp)
    lw      s0, F_S0(sp)
    lw      s1, F_S1(sp)
    lw      a0, F_A0(sp)
    lw      a1, F_A1(sp)
    lw      a2, F_A2(sp)
    lw      a3, F_A3(sp)
    lw      a4, F_A4(sp)
    lw      a5, F_A5(sp)
    lw      a6, F_A6(sp)
    lw      a7, F_A7(sp)
    lw      s2, F_S2(sp)
    lw      s3, F_S3(sp)
    lw      s4, F_S4(sp)
    lw      s5, F_S5(sp)
    lw      s6, F_S6(sp)
    lw      s7, F_S7(sp)
    lw      s8, F_S8(sp)
    lw      s9, F_S9(sp)
    lw      s10, F_S10(sp)
    lw      s11, F_S11(sp)
    lw      t3, F_T3(sp)
    lw      t4, F_T4(sp)
    lw      t5, F_T5(sp)
    lw      t6, F_T6(sp)
    addi    sp, sp, FRAME_SIZE
    mret
    .size f101_trap_entry, . - f101_trap_entry

/* void vPortStartFirstTask(void): run the task pxCurrentTCB points at */
    .global vPortStartFirstTask
    .type   vPortStartFirstTask, @function
vPortStartFirstTask:
    la      t0, pxCurrentTCBs
    lw      t0, 0(t0)
    lw      sp, 0(t0)
    li      t0, 1
    la      t1, port_xSchedulerRunning
    sw      t0, 0(t1)
    j       6b
    .size vPortStartFirstTask, . - vPortStartFirstTask

    .global vPortYield
    .type   vPortYield, @function
vPortYield:
    ecall
    ret
    .size vPortYield, . - vPortYield
