#ifndef __TK_SYSDEF_DEPEND_CPU_H__
#define __TK_SYSDEF_DEPEND_CPU_H__

#include "../../../machine.h"
#include "../core/riscv_esp/sysdef.h"

/* On-chip SRAM (DRAM view) */
#define INTERNAL_RAM_START	0x3FC80000
#define INTERNAL_RAM_SIZE	0x00050000	/* 320 KB */
#define INTERNAL_RAM_END	(INTERNAL_RAM_START + INTERNAL_RAM_SIZE)

#define INITIAL_SP		INTERNAL_RAM_END	/* IDF actually owns this */

/* System timer */
#define MIN_TIMER_PERIOD	1
#define MAX_TIMER_PERIOD	50

/* CPU clock (set by IDF; for TMCLK math only) */
#define	SYSCLK			160		/* MHz */
#define	TMCLK			SYSCLK
#define	TMCLK_KHz		(TMCLK * 1000)

/* Interrupt vectors: routed via IDF's interrupt matrix */
#define N_INTVEC		62		/* Peripheral interrupt sources on C3 */
#define N_SYSVEC		16		/* Reserved; unused under IDF */
#define INTPRI_BITWIDTH		4

/* Coprocessors */
#define CPU_HAS_FPU		0
#define CPU_HAS_DSP		0
#define NUM_COPROCESSOR		0

/* Physical timer: hardware exists; runtime impl deferred to a later stage.
   Setting to 1 so config.h's USE_PTMR=1 default doesn't trip the knldef.h sanity check. */
#define CPU_HAS_PTMR		1

#endif
