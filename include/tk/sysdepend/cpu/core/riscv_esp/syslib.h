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

#ifndef __TK_SYSLIB_DEPEND_CORE_H__
#define __TK_SYSLIB_DEPEND_CORE_H__

#include <tk/errno.h>
#include <sys/sysdef.h>

IMPORT UW disint(void);			/* Returns prior mstatus.MIE state, then clears it */
IMPORT void enaint(UW intsts);		/* Restores mstatus.MIE from saved state */

#define DI(intsts)	( (intsts) = disint() )
#define EI(intsts)	( enaint(intsts) )
#define isDI(intsts)	( ((intsts) & MSTATUS_MIE) == 0 )

/*
 * ESP RISC-V CPU interrupt levels are backed by the interrupt threshold
 * register.  Threshold 1 is IDF's normal enabled state; threshold 4 masks the
 * C-callable LEVEL1..LEVEL3 interrupt handlers used by this port.
 *
 * SetCpuIntLevel() contract on this port:
 *   - Levels are threshold values: INTLEVEL_EI (1) = all interrupts enabled,
 *     INTLEVEL_DI (4) = all C-serviceable interrupts (LEVEL1..LEVEL3) masked.
 *   - Levels are per-task state: the threshold is saved into the task's
 *     context block on every switch and restored when the task resumes, so
 *     a level of 2..3 survives preemption.  (At level 3 the LEVEL2 kernel
 *     tick is masked while that task runs; elapsed ticks are made up by the
 *     timer catch-up when the level drops.)
 *   - Blocking (dispatch-invoking) calls at ANY level above INTLEVEL_EI are
 *     rejected with E_CTX -- the LEVEL1 dispatch interrupt could not rescue
 *     a task that blocked with it masked.  Non-blocking calls (tk_wup_tsk,
 *     tk_sig_sem, ...) are allowed; a resulting preemption is latched and
 *     fires as soon as the level returns to INTLEVEL_EI (likewise for
 *     wakeups issued under DI(): they dispatch at EI(), not a tick later).
 *   - Exception: kernel paths that must fully enable interrupts internally
 *     (device-management calls, fixed-size memory pool waits, and the
 *     immediate handler invocation in tk_sta_cyc/tk_sta_alm) reset the
 *     level to INTLEVEL_EI.  Do not hold 2..3 across those calls.
 */
#define INTLEVEL_EI	(1)
#define INTLEVEL_DI	(4)

#define DINTNO(intvec)	(intvec)

/*
 * tk_def_int() intno semantics on this port:
 *
 *   intno is an Espressif ETS_*_SOURCE enum value (see soc/interrupts.h),
 *   not an NVIC vector number.  EnableInt(intno, level) wires the handler
 *   through IDF's interrupt matrix via esp_intr_alloc() at LEVEL1..LEVEL3, so
 *   it runs in C with caller-saved registers already preserved by the trap
 *   dispatcher.
 *
 *   Limitations: SetIntMode() is not supported on this port, and EDGE/IRAM
 *   esp_intr_alloc flags are not exposed through T_DINT.intatr.  Drivers
 *   needing edge-triggered, IRAM-resident, or high-priority assembly handlers
 *   should call esp_intr_alloc() directly.
 *
 *   The interrupt-matrix slot is allocated eagerly by tk_def_int() at LEVEL1
 *   (disabled), so allocation failure surfaces as tk_def_int()'s return code
 *   and EnableInt(intno, 1) is a pure unmask, safe from any context.
 *   EnableInt() at level 2..3 re-allocates the slot on its first call and
 *   must therefore be issued from task context (from an ISR it is refused
 *   and logged to the ROM console).  Each defined source holds one of the
 *   31 routable CPU interrupt slots until redefined with inthdr == NULL.
 *
 *   CheckInt(intno) returns the source's raw request-line status from the
 *   interrupt matrix (INTERRUPT_CORE0_INTR_STATUS_n_REG), independent of
 *   whether the source is defined, enabled, or masked by the threshold.
 */

#endif
