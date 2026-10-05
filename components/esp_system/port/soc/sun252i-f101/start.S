/*
 * SPDX-FileCopyrightText: 2026 Yuzuki Tsuru
 * SPDX-License-Identifier: Apache-2.0
 *
 * Entry of the image after "xfel exec": machine mode, caches in the state the
 * boot ROM left them, MIE possibly set.
 */
#include "sdkconfig.h"

    .section .exception_vectors.text, "ax"     /* first in the image: entry at the load address */
    .globl call_start_cpu0
    .type call_start_cpu0, @function
call_start_cpu0:
    csrci mstatus, 8
    li    t0, 0x2000
    csrs  mstatus, t0         /* FPU state initial: libgcc soft-float reads the rounding mode */
    csrw  mie, zero
    csrw  mip, zero
    /* C907 extension CSRs: instruction/data cache, prefetch, branch predictors */
    li    t0, 0x70013
    csrw  0x7c2, t0           /* mcor */
    li    t0, 0x11ff
    csrw  0x7c1, t0           /* mhcr */
    li    t0, 0x16e30c
    csrw  0x7c5, t0           /* mhint */
    fence
    fence.i
    la    t0, f101_early_trap
    csrw  mtvec, t0
    .option push
    .option norelax
    la    gp, __global_pointer$
    .option pop
    la    sp, _startup_stack_top
    la    t0, _bss_start
    la    t1, _bss_end
1:  bgeu  t0, t1, 2f
    sw    zero, 0(t0)
    addi  t0, t0, 4
    j     1b
2:  call  call_start_cpu0_c
3:  j     3b
    .size call_start_cpu0, . - call_start_cpu0

    .section .bss.startup_stack, "aw", @nobits
    .balign 16
    .globl _startup_stack_bottom
_startup_stack_bottom:
    .space CONFIG_ESP_MAIN_TASK_STACK_SIZE
    .globl _startup_stack_top
_startup_stack_top:

/* exception before the scheduler took over mtvec: print mcause/mepc/mtval/ra,
 * the string a0 points at (abort details) and some stack words, then stop */
    .section .text.f101_early_trap, "ax"
    .balign 4
    .globl f101_early_trap
f101_early_trap:
    mv    s1, ra
    mv    s2, sp
    mv    s4, a0
    li    a0, '!'
    jal   ra, putc
    li    a0, 'T'
    jal   ra, putc
    li    a0, 10
    jal   ra, putc
    csrr  a0, mcause
    jal   ra, puthex
    csrr  a0, mepc
    jal   ra, puthex
    csrr  a0, mtval
    jal   ra, puthex
    mv    a0, s1
    jal   ra, puthex
    li    t0, 0x40010000
    bltu  s4, t0, 6f
    li    t0, 0x41000000
    bgeu  s4, t0, 6f
    li    s3, 160
5:  lbu   a0, 0(s4)
    beqz  a0, 6f
    jal   ra, putc
    addi  s4, s4, 1
    addi  s3, s3, -1
    bnez  s3, 5b
6:  li    a0, 10
    jal   ra, putc
    li    s3, 24
7:  lw    a0, 0(s2)
    addi  s2, s2, 4
    jal   ra, puthex
    addi  s3, s3, -1
    bnez  s3, 7b
8:  j     8b

putc:
    li    t2, 0x02500c00
0:  lw    t3, 0x14(t2)
    andi  t3, t3, 0x20
    beqz  t3, 0b
    sw    a0, 0(t2)
    ret

/* a0 as 8 hex digits and a newline */
puthex:
    mv    t6, ra
    mv    t4, a0
    li    t5, 8
1:  srli  a0, t4, 28
    slli  t4, t4, 4
    li    t0, 10
    blt   a0, t0, 2f
    addi  a0, a0, 'a' - 10 - '0'
2:  addi  a0, a0, '0'
    jal   ra, putc
    addi  t5, t5, -1
    bnez  t5, 1b
    li    a0, 10
    jal   ra, putc
    mv    ra, t6
    ret
