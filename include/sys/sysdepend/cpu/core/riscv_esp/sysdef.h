/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.00.07.B0
 *
 *    Copyright (C) 2006-2023 by Ken Sakamura.
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 *
 *    Released by TRON Forum(http://www.tron.org) at 2023/11.
 *
 *----------------------------------------------------------------------
 */

#ifndef __SYS_SYSDEF_DEPEND_CORE_H__
#define __SYS_SYSDEF_DEPEND_CORE_H__

/* RISC-V machine-mode status bits used in inline asm.
   Guarded because IDF's riscv/encoding.h defines the same names. */
#ifndef MSTATUS_MIE
#define MSTATUS_MIE	(1U << 3)
#endif
#ifndef MSTATUS_MPIE
#define MSTATUS_MPIE	(1U << 7)
#endif
#ifndef MSTATUS_MPP
#define MSTATUS_MPP	(3U << 11)
#endif

#define MIN_SYS_STACK_SIZE	256
#define DEFAULT_SYS_STKSZ	MIN_SYS_STACK_SIZE

/*
 * Threshold value written by ENABLE_INTERRUPT_UPTO() while cyclic/alarm user
 * handlers run inside the tick ISR (see knl_call_cychdr/knl_call_almhdr).
 * The ESP32-C3 INTC fires interrupts with priority >= threshold, and the
 * kernel tick is allocated at ESP_INTR_FLAG_LEVEL2 (sys_timer.c), so masking
 * the tick and everything below it requires threshold 3: LEVEL3 interrupts
 * still preempt the handlers, LEVEL1/LEVEL2 (including the tick itself) wait.
 * Value 1 would unmask the tick and allow it to nest into its own handler
 * whenever a cyclic/alarm handler overruns one tick period.
 */
#define TIMER_INTLEVEL		3

#endif
