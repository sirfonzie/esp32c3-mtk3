#ifndef _SYSDEPEND_CPU_CORE_SYSTIMER_
#define _SYSDEPEND_CPU_CORE_SYSTIMER_

/* Real bodies live in sys_timer.c and use the ESP SYSTIMER peripheral via
 * esp_intr_alloc.  These inlines keep the common kernel timer layer isolated
 * from the ESP-specific implementation names. */

IMPORT void knl_start_hw_timer_impl(void);
IMPORT void knl_clear_hw_timer_interrupt_impl(void);
IMPORT void knl_end_of_hw_timer_interrupt_impl(void);
IMPORT void knl_terminate_hw_timer_impl(void);
IMPORT UW   knl_get_hw_timer_nsec_impl(void);

Inline void knl_start_hw_timer(void)         { knl_start_hw_timer_impl(); }
Inline void knl_clear_hw_timer_interrupt(void){ knl_clear_hw_timer_interrupt_impl(); }
Inline void knl_end_of_hw_timer_interrupt(void){ knl_end_of_hw_timer_interrupt_impl(); }
Inline void knl_terminate_hw_timer(void)     { knl_terminate_hw_timer_impl(); }
Inline UW   knl_get_hw_timer_nsec(void)      { return knl_get_hw_timer_nsec_impl(); }

#endif
