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

#ifndef _SYSDEPEND_CPU_CORE_CPUTASK_
#define _SYSDEPEND_CPU_CORE_CPUTASK_

/*
 * Saved task context on the task's own stack.
 *
 * Layout MUST match the first 128 bytes of IDF's RvExcFrame
 * (components/riscv/include/riscv/rvruntime-frames.h), because IDF's
 * trap entry pushes / trap exit pops exactly CONTEXT_SIZE = 32*4 = 128 bytes
 * with these field offsets.  The trailing mstatus/mtvec/mcause/mtval/mhartid
 * fields of RvExcFrame are only populated for exception entries, not the
 * standard interrupt path we use for dispatch.
 *
 * Keeping sizeof(SStackFrame) == 128 also means knl_setup_context's
 * (isstack - sizeof) initial sp lands exactly where IDF's restore expects it.
 */
typedef struct {
	UW	mepc;
	UW	ra;		/* x1 */
	UW	sp;		/* x2 - not actually restored from here; sp IS sp */
	UW	gp;		/* x3 */
	UW	tp;		/* x4 */
	UW	t0;		/* x5 */
	UW	t1;		/* x6 */
	UW	t2;		/* x7 */
	UW	s0;		/* x8 / fp */
	UW	s1;		/* x9 */
	UW	a0;		/* x10 - task entry arg 0 (stacd) */
	UW	a1;		/* x11 - task entry arg 1 (exinf) */
	UW	a2;
	UW	a3;
	UW	a4;
	UW	a5;
	UW	a6;
	UW	a7;
	UW	s2;
	UW	s3;
	UW	s4;
	UW	s5;
	UW	s6;
	UW	s7;
	UW	s8;
	UW	s9;
	UW	s10;
	UW	s11;
	UW	t3;
	UW	t4;
	UW	t5;
	UW	t6;
} SStackFrame;

_Static_assert(sizeof(SStackFrame) == 128, "SStackFrame must match IDF CONTEXT_SIZE");

#define DORMANT_STACK_SIZE	(sizeof(SStackFrame))

Inline void knl_setup_context(TCB *tcb)
{
	SStackFrame *ssp;

	ssp = (SStackFrame*)tcb->isstack - 1;
	for (UW *p = (UW*)ssp; p < (UW*)tcb->isstack; p++) *p = 0;

	ssp->mepc = (UW)tcb->task;

	extern UW __global_pointer$;
	ssp->gp = (UW)&__global_pointer$;

	/* Inherit tp (thread pointer) from the creating context.  Newlib TLS
	 * variables (_tls_errno etc.) are accessed as tp+offset.  This preserves
	 * IDF startup TLS, but it does not provide per-task TLS isolation. */
	UW _cur_tp;
	__asm__ volatile("mv %0, tp" : "=r"(_cur_tp));
	if (_cur_tp) ssp->tp = _cur_tp;

	tcb->tskctxb.ssp = ssp;
	tcb->tskctxb.intlevel = INTLEVEL_EI;	/* tasks start fully enabled */
}

Inline void knl_setup_stacd(TCB *tcb, INT stacd)
{
	SStackFrame *ssp = (SStackFrame*)tcb->tskctxb.ssp;
	ssp->a0 = (UW)stacd;
	ssp->a1 = (UW)tcb->exinf;
}

Inline void knl_cleanup_context(TCB *tcb)
{
	(void)tcb;
}

#endif
