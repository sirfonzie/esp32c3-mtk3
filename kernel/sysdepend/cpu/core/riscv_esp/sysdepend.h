#ifndef _SYSDEPEND_CPU_CORE_SYSDEPEND_
#define _SYSDEPEND_CPU_CORE_SYSDEPEND_

/* System-timer ISR and elapsed-period accounting implemented in sys_timer.c. */
IMPORT void knl_systim_inthdr(void);
IMPORT UW knl_get_hw_timer_elapsed_periods_impl(void);
IMPORT void knl_clear_hw_timer_interrupt_impl(void);

/*
 * Task context block.
 *
 * ssp:      saved stack pointer (points at the task's saved SStackFrame).
 * intlevel: the task's CPU interrupt threshold (SetCpuIntLevel value) at the
 *           moment it was switched away from; written back to the INTC
 *           threshold register when the task is resumed, so levels set with
 *           SetCpuIntLevel survive preemption.  __wrap_rtos_int_exit
 *           (cpu_cntl.c) accesses both fields by offset from inline asm --
 *           keep ssp first and intlevel immediately after it.
 */
typedef struct {
	void	*ssp;
	UW	intlevel;
} CTXB;

#endif
