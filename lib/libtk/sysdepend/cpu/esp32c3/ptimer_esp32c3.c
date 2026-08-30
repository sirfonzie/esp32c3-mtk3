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

/*
 *	ptimer_esp32c3.c
 *
 *	Physical timer (ESP32-C3)
 *
 *	Backs the tk_*ptmr API with Timer Group 0 / Timer Group 1, each
 *	driving its single timer (T0).  Source clock is XTAL (40 MHz); a
 *	prescaler of 40 gives a 1 us tick across both physical timers, so
 *	the `limit` argument to StartPhysicalTimer is in microseconds.
 */

#include <sys/machine.h>
#ifdef CPU_ESP32C3

#include <tk/tkernel.h>
#include <tk/syslib.h>

#if USE_PTMR

#include <stdbool.h>
#include "soc/interrupts.h"
#include "soc/periph_defs.h"
#include "esp_private/periph_ctrl.h"
#include "hal/timer_ll.h"

#define PTMR_PRESCALE	40U			/* XTAL 40 MHz / 40 = 1 MHz */
#define PTMR_TICK_HZ	1000000U		/* tick rate after prescale */

typedef struct {
	int		group_id;	/* TIMG group (0 or 1) */
	UINT		intno;		/* ETS_*_SOURCE for this group's T0 */
	UINT		mode;		/* TA_ALM_PTMR or TA_CYC_PTMR, or -1 idle */
	FP		hdr;		/* User handler */
	void		*exinf;
} T_PTMRCB;

LOCAL T_PTMRCB ptmrcb[TK_MAX_PTIMER] = {
	{ .group_id = 0, .intno = ETS_TG0_T0_LEVEL_INTR_SOURCE,
	  .mode = (UINT)-1, .hdr = NULL, .exinf = NULL },
	{ .group_id = 1, .intno = ETS_TG1_T0_LEVEL_INTR_SOURCE,
	  .mode = (UINT)-1, .hdr = NULL, .exinf = NULL },
};

LOCAL timg_dev_t *cb_hw(const T_PTMRCB *cb) { return TIMER_LL_GET_HW(cb->group_id); }

/*
 * Per-group ISR.  Clears the T0 alarm latch, invokes the user handler, and
 * for one-shot mode masks the source so it stays quiet until the next start.
 * The hardware itself stops generating new alarms after a one-shot fire
 * because we leave auto-reload off, but the source enable bit is still hot --
 * disabling it here mirrors the stm32l4 port's behaviour and prevents the
 * latch from re-firing if some bystander rewrites the alarm value.
 */
LOCAL void ptmr_int_main(UINT intno, T_PTMRCB *cb)
{
	timg_dev_t *hw = cb_hw(cb);

	timer_ll_clear_intr_status(hw, TIMER_LL_EVENT_ALARM(0));

	if (cb->hdr != NULL) {
		(*cb->hdr)(cb->exinf);
	}

	if (cb->mode == TA_ALM_PTMR) {
		DisableInt(intno);
		timer_ll_enable_alarm(hw, 0, false);
	} else {
		/* TA_CYC_PTMR: re-arm.  Auto-reload zeros the counter on alarm,
		 * but the alarm enable is single-shot per fire on this peripheral. */
		timer_ll_enable_alarm(hw, 0, true);
	}
}

LOCAL void ptmr1_inthdr(UINT intno) { ptmr_int_main(intno, &ptmrcb[0]); }
LOCAL void ptmr2_inthdr(UINT intno) { ptmr_int_main(intno, &ptmrcb[1]); }

LOCAL FP const inthdr_tbl[TK_MAX_PTIMER] = { (FP)ptmr1_inthdr, (FP)ptmr2_inthdr };

LOCAL void ptmr_hw_init(T_PTMRCB *cb)
{
	timg_dev_t *hw = cb_hw(cb);

	periph_module_enable((cb->group_id == 0)
		? PERIPH_TIMG0_MODULE : PERIPH_TIMG1_MODULE);

	timer_ll_enable_clock(cb->group_id, 0, true);
	timer_ll_set_clock_source(cb->group_id, 0, GPTIMER_CLK_SRC_XTAL);
	timer_ll_set_count_direction(hw, 0, GPTIMER_COUNT_UP);
	timer_ll_set_clock_prescale(hw, 0, PTMR_PRESCALE);

	timer_ll_enable_alarm(hw, 0, false);
	timer_ll_enable_intr(hw, TIMER_LL_EVENT_ALARM(0), false);
	timer_ll_clear_intr_status(hw, TIMER_LL_EVENT_ALARM(0));
}

EXPORT ER StartPhysicalTimer(UINT ptmrno, UW limit, UINT mode)
{
	if (ptmrno == 0 || ptmrno > TK_MAX_PTIMER) return E_PAR;
	if (limit == 0 || mode > TA_CYC_PTMR) return E_PAR;

	T_PTMRCB *cb = &ptmrcb[ptmrno - 1];
	timg_dev_t *hw = cb_hw(cb);
	T_DINT dint = { .intatr = TA_HLNG, .inthdr = inthdr_tbl[ptmrno - 1] };
	ER err;

	cb->mode = mode;

	ptmr_hw_init(cb);

	timer_ll_enable_counter(hw, 0, false);
	timer_ll_set_reload_value(hw, 0, 0);
	timer_ll_trigger_soft_reload(hw, 0);
	timer_ll_set_alarm_value(hw, 0, (uint64_t)limit);
	timer_ll_enable_auto_reload(hw, 0, mode == TA_CYC_PTMR);

	err = tk_def_int(cb->intno, &dint);
	if (err != E_OK) return err;

	timer_ll_clear_intr_status(hw, TIMER_LL_EVENT_ALARM(0));
	timer_ll_enable_intr(hw, TIMER_LL_EVENT_ALARM(0), true);
	timer_ll_enable_alarm(hw, 0, true);
	EnableInt(cb->intno, 1);

	timer_ll_enable_counter(hw, 0, true);
	return E_OK;
}

EXPORT ER StopPhysicalTimer(UINT ptmrno)
{
	if (ptmrno == 0 || ptmrno > TK_MAX_PTIMER) return E_PAR;

	T_PTMRCB *cb = &ptmrcb[ptmrno - 1];
	timg_dev_t *hw = cb_hw(cb);

	DisableInt(cb->intno);
	timer_ll_enable_counter(hw, 0, false);
	timer_ll_enable_alarm(hw, 0, false);
	timer_ll_enable_intr(hw, TIMER_LL_EVENT_ALARM(0), false);
	timer_ll_clear_intr_status(hw, TIMER_LL_EVENT_ALARM(0));
	cb->mode = (UINT)-1;
	return E_OK;
}

EXPORT ER GetPhysicalTimerCount(UINT ptmrno, UW *p_count)
{
	if (ptmrno == 0 || ptmrno > TK_MAX_PTIMER) return E_PAR;
	if (p_count == NULL) return E_PAR;

	timg_dev_t *hw = cb_hw(&ptmrcb[ptmrno - 1]);

	/* Counter lives in a different clock domain; soft_capture latches the
	 * current value into the readable hi/lo registers atomically. */
	timer_ll_trigger_soft_capture(hw, 0);
	*p_count = (UW)timer_ll_get_counter_value(hw, 0);
	return E_OK;
}

EXPORT ER DefinePhysicalTimerHandler(UINT ptmrno, CONST T_DPTMR *pk_dptmr)
{
	if (ptmrno == 0 || ptmrno > TK_MAX_PTIMER) return E_PAR;

	T_PTMRCB *cb = &ptmrcb[ptmrno - 1];
	if (pk_dptmr != NULL) {
		cb->hdr   = pk_dptmr->ptmrhdr;
		cb->exinf = pk_dptmr->exinf;
	} else {
		cb->hdr   = NULL;
		cb->exinf = NULL;
	}
	return E_OK;
}

EXPORT ER GetPhysicalTimerConfig(UINT ptmrno, T_RPTMR *pk_rptmr)
{
	if (ptmrno == 0 || ptmrno > TK_MAX_PTIMER) return E_PAR;
	if (pk_rptmr == NULL) return E_PAR;

	pk_rptmr->ptmrclk  = PTMR_TICK_HZ;
	pk_rptmr->maxcount = 0xFFFFFFFFU;	/* API limit field is UW (32-bit) */
	pk_rptmr->defhdr   = TRUE;
	return E_OK;
}

#endif	/* USE_PTMR */
#endif	/* CPU_ESP32C3 */
