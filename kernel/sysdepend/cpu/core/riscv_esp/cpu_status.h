#ifndef _SYSDEPEND_CPU_CORE_STATUS_
#define _SYSDEPEND_CPU_CORE_STATUS_

#include <tk/syslib.h>
#include <sys/sysdef.h>

#include "sysdepend.h"

/*
 * Critical sections: disable interrupts, run, then conditionally invoke the
 * dispatcher if a task switch became necessary while interrupts were off.
 *
 * The dispatch request is issued even when the section was entered with
 * interrupts already masked (user DI(), nested sections, SetCpuIntLevel>EI):
 * knl_dispatch() only *pends* the FROM_CPU_INTR0 software interrupt, which
 * stays latched in hardware and fires the moment MIE and the threshold allow
 * it.  A wakeup issued under DI() therefore dispatches immediately at EI()
 * instead of waiting for the next tick.  (Self-blocking calls cannot reach
 * this point with interrupts masked -- CHECK_DISPATCH rejects them with
 * E_CTX first -- so knl_dispatch()'s schedtsk==NULL WFI loop, which briefly
 * re-enables interrupts, only runs for legal blocking calls.)
 */
#define BEGIN_CRITICAL_SECTION	{ UW _mie_ = disint();
#define END_CRITICAL_SECTION	if ( knl_ctxtsk != knl_schedtsk		\
				  && !knl_dispatch_disabled ) {		\
					knl_dispatch();			\
				}					\
				enaint(_mie_); }

#define BEGIN_DISABLE_INTERRUPT	{ UW _mie_ = disint();
#define END_DISABLE_INTERRUPT	enaint(_mie_); }

#define ENABLE_INTERRUPT	{ SetCpuIntLevel(INTLEVEL_EI); enaint(MSTATUS_MIE); }
#define DISABLE_INTERRUPT	{ (void)disint(); }

#define ENABLE_INTERRUPT_UPTO(level)	{ SetCpuIntLevel(level); enaint(MSTATUS_MIE); }

/* Task-independent (ISR) nesting depth. */
IMPORT W knl_taskindp;
IMPORT W knl_isr_nest;

Inline BOOL knl_isTaskIndependent(void)
{
	return (knl_taskindp > 0) ? TRUE : FALSE;
}

Inline void knl_EnterTaskIndependent(void) { knl_taskindp++; }
Inline void knl_LeaveTaskIndependent(void) { knl_taskindp--; }

#define ENTER_TASK_INDEPENDENT	{ knl_EnterTaskIndependent(); }
#define LEAVE_TASK_INDEPENDENT	{ knl_LeaveTaskIndependent(); }

/* Read current CPU interrupt state for in_loc()/in_ddsp() checks. */
Inline UW knl_get_mstatus(void)
{
	UW v;
	Asm("csrr %0, mstatus" : "=r"(v));
	return v;
}

Inline BOOL knl_is_cpu_int_masked(void)
{
	return (isDI(knl_get_mstatus()) || GetCpuIntLevel() >= INTLEVEL_DI) ? TRUE : FALSE;
}

/*
 * Dispatch is additionally masked at intermediate thresholds 2..3: the
 * FROM_CPU_INTR0 dispatch interrupt is LEVEL1, so a task that blocks while
 * the threshold is above INTLEVEL_EI could not be switched away from (at
 * threshold 3 not even the LEVEL2 tick could rescue it) and would keep
 * executing in WAIT state.  Blocking calls at any raised level are
 * therefore rejected with E_CTX, exactly as at INTLEVEL_DI.
 */
Inline BOOL knl_is_dispatch_masked(void)
{
	return (isDI(knl_get_mstatus()) || GetCpuIntLevel() > INTLEVEL_EI) ? TRUE : FALSE;
}

#define in_indp()	( knl_isTaskIndependent() || knl_ctxtsk == NULL )

#define in_ddsp()	( knl_dispatch_disabled	\
			|| in_indp()		\
			|| knl_is_dispatch_masked() )

#define in_loc()	( knl_is_cpu_int_masked()	\
			|| in_indp() )

#define in_qtsk()	( knl_ctxtsk->sysmode > knl_ctxtsk->isysmode )

#endif
