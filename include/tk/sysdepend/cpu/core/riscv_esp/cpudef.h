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

#ifndef __TK_CPUDEF_CORE_H__
#define __TK_CPUDEF_CORE_H__

#define TA_COPS		0
#define TA_FPU		TA_COP0		/* dummy; FPU not present on RV32IMC */

/* General-purpose registers visible via tk_get_reg/tk_set_reg.
 * RISC-V has x1..x31; x0 is hardwired zero. */
typedef struct t_regs {
	VW	x[31];		/* x1..x31 */
} T_REGS;

typedef struct t_eit {
	void	*pc;		/* mepc */
	UW	mstatus;
	UW	taskmode;
} T_EIT;

typedef struct t_cregs {
	void	*ssp;		/* Task stack pointer */
} T_CREGS;

#endif
