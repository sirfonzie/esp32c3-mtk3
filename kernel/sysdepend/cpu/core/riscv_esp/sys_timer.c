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

#include <sys/machine.h>
#ifdef CPU_CORE_RISCV_ESP

#include "kernel.h"

#include "esp_intr_alloc.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_rom_sys.h"
#include "esp_private/systimer.h"
#include "soc/interrupts.h"
#include "soc/systimer_struct.h"
#include "hal/systimer_ll.h"

/*
 * SYSTIMER alarm 0 / OS tick counter drives the micro T-Kernel system tick.
 *
 * ESP32-C3 SYSTIMER counter clocks at XTAL/2.5 = 16 MHz today, but use IDF's
 * target conversion helpers instead of baking that rate into the port.  The
 * counter is 52-bit; we run it free, with the alarm in periodic mode firing
 * every TIMER_PERIOD (= CNF_TIMER_PERIOD, default 10) milliseconds.
 *
 * These IDs match ESP-IDF's static resource split: counter 0 is the esp_timer
 * wall clock, counter 1 is the OS tick source, and alarm 0 is core 0's tick.
 */

#define SYSTIM_COUNTER_ID	SYSTIMER_COUNTER_OS_TICK
#define SYSTIM_ALARM_ID		SYSTIMER_ALARM_OS_TICK_CORE0

static intr_handle_t systim_intr_handle = NULL;
static uint32_t systim_period_ticks;
static uint64_t systim_last_tick_ticks;

static void IRAM_ATTR systim_isr(void *arg)
{
	(void)arg;
	knl_systim_inthdr();
}

EXPORT void knl_start_hw_timer_impl(void)
{
	systimer_dev_t *dev = &SYSTIMER;
	esp_err_t err;

	systim_period_ticks = (uint32_t)systimer_us_to_ticks((uint64_t)TIMER_PERIOD * 1000ULL);
	systim_last_tick_ticks = 0;

	systimer_ll_enable_counter(dev, SYSTIM_COUNTER_ID, false);
	systimer_ll_enable_alarm(dev, SYSTIM_ALARM_ID, false);
	systimer_ll_enable_alarm_int(dev, SYSTIM_ALARM_ID, false);
	systimer_ll_set_counter_value(dev, SYSTIM_COUNTER_ID, 0);
	systimer_ll_apply_counter_value(dev, SYSTIM_COUNTER_ID);

	systimer_ll_connect_alarm_counter(dev, SYSTIM_ALARM_ID, SYSTIM_COUNTER_ID);
	systimer_ll_set_alarm_period(dev, SYSTIM_ALARM_ID, systim_period_ticks);
	systimer_ll_enable_alarm_period(dev, SYSTIM_ALARM_ID);
	systimer_ll_apply_alarm_value(dev, SYSTIM_ALARM_ID);

	/* No ESP_INTR_FLAG_IRAM: knl_systim_inthdr and the timer chain are in
	 * flash.  The IDF SPI flash driver will auto-mask this interrupt during
	 * flash operations, avoiding the cache-disabled fault.
	 *
	 * LEVEL2 here and TIMER_INTLEVEL (sysdef.h) are a matched pair: the
	 * threshold TIMER_INTLEVEL must be exactly one above this level so the
	 * tick stays masked during cyclic/alarm user handlers. */
	err = esp_intr_alloc(ETS_SYSTIMER_TARGET0_EDGE_INTR_SOURCE,
	                     ESP_INTR_FLAG_LEVEL2,
	                     systim_isr, NULL, &systim_intr_handle);
	ESP_ERROR_CHECK(err);

	systimer_ll_enable_counter(dev, SYSTIM_COUNTER_ID, true);
	systimer_ll_enable_alarm(dev, SYSTIM_ALARM_ID, true);
	systimer_ll_enable_alarm_int(dev, SYSTIM_ALARM_ID, true);
}

EXPORT void knl_clear_hw_timer_interrupt_impl(void)
{
	systimer_ll_clear_alarm_int(&SYSTIMER, SYSTIM_ALARM_ID);
}

EXPORT void knl_end_of_hw_timer_interrupt_impl(void) { }

EXPORT void knl_terminate_hw_timer_impl(void)
{
	systimer_ll_enable_alarm(&SYSTIMER, SYSTIM_ALARM_ID, false);
	systimer_ll_enable_alarm_int(&SYSTIMER, SYSTIM_ALARM_ID, false);
	if (systim_intr_handle != NULL) {
		esp_intr_free(systim_intr_handle);
		systim_intr_handle = NULL;
	}
}

static uint64_t systim_get_counter_ticks(void)
{
	systimer_dev_t *dev = &SYSTIMER;
	uint32_t lo, lo_start, hi;

	systimer_ll_counter_snapshot(dev, SYSTIM_COUNTER_ID);
	while (!systimer_ll_is_counter_value_valid(dev, SYSTIM_COUNTER_ID)) {
	}

	lo_start = systimer_ll_get_counter_value_low(dev, SYSTIM_COUNTER_ID);
	do {
		lo = lo_start;
		hi = systimer_ll_get_counter_value_high(dev, SYSTIM_COUNTER_ID);
		lo_start = systimer_ll_get_counter_value_low(dev, SYSTIM_COUNTER_ID);
	} while (lo_start != lo);

	return ((uint64_t)hi << 32) | lo;
}

/*
 * Catch-up bound: at most one second's worth of missed periods is replayed
 * per query.  A stall longer than that (flash erase storms, a runaway
 * handler, a debugger halt) is credited up to the cap and the remainder is
 * dropped -- kernel time slips by the dropped amount instead of the tick ISR
 * looping for an unbounded burst.  Sustained overload (every period's
 * handler work exceeding the period) still accumulates deficit each tick and
 * ends in the INT-WDT panic, which is the correct outcome for a genuine
 * runaway; the cap only bounds the damage of a one-shot stall.  Slips are
 * counted in knl_systim_slipped_periods and logged.
 */
#define SYSTIM_MAX_CATCHUP_PERIODS	((UW)((1000 + TIMER_PERIOD - 1) / TIMER_PERIOD))

EXPORT UW knl_systim_slipped_periods = 0;

EXPORT UW knl_get_hw_timer_elapsed_periods_impl(void)
{
	uint64_t now_ticks;
	uint64_t elapsed_ticks;
	UW periods;

	if (systim_period_ticks == 0) {
		return 1;
	}

	now_ticks = systim_get_counter_ticks();
	elapsed_ticks = now_ticks - systim_last_tick_ticks;
	periods = (UW)(elapsed_ticks / systim_period_ticks);

	if (periods > SYSTIM_MAX_CATCHUP_PERIODS) {
		UW slip = periods - SYSTIM_MAX_CATCHUP_PERIODS;
		knl_systim_slipped_periods += slip;
		systim_last_tick_ticks += (uint64_t)slip * systim_period_ticks;
		periods = SYSTIM_MAX_CATCHUP_PERIODS;
		esp_rom_printf("[mtkernel] tick stall: dropped %u periods (%u total); kernel time slipped\n",
		               (unsigned)slip, (unsigned)knl_systim_slipped_periods);
	}

	/* periods == 0 means an early/spurious wake (e.g. an alarm edge that
	 * landed after a catch-up pass already credited its period).  Report 0
	 * and let the caller just acknowledge the interrupt: forcing 1 here
	 * would advance systim_last_tick_ticks past the hardware counter and
	 * permanently skew system time by one period. */
	systim_last_tick_ticks += (uint64_t)periods * systim_period_ticks;
	return periods;
}

/*
 * Sub-tick elapsed-time getter.  Returned value must satisfy:
 *   0 <= ns < 2 * TIMER_PERIOD * 1e6
 * Used by debugger time-reference calls for high-resolution time.
 */
EXPORT UW knl_get_hw_timer_nsec_impl(void)
{
	uint32_t ticks_per_us;
	uint32_t elapsed_ticks;

	if (systim_period_ticks == 0) {
		return 0;
	}

	ticks_per_us = (uint32_t)systimer_us_to_ticks(1);
	elapsed_ticks = (uint32_t)((systim_get_counter_ticks() - systim_last_tick_ticks)
	                           % systim_period_ticks);

	return (UW)(((uint64_t)elapsed_ticks * 1000ULL) / ticks_per_us);
}

#endif
