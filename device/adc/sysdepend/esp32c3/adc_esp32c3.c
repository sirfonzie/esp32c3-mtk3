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
 *	adc_esp32c3.c
 *	A/D converter device driver
 *	System dependent processing for ESP32-C3
 *
 *	ESP32-C3's ADC uses the digital controller for one-shot reads -- no
 *	separate RTC controller exists.  Conversions are microseconds, so we
 *	poll inside adc_oneshot_hal_convert and avoid the interrupt path
 *	entirely (no tk_def_int call here, in contrast to the UART driver).
 */

#include <sys/machine.h>
#ifdef CPU_ESP32C3

#include <stdbool.h>
#include <tk/tkernel.h>
#include "../../adc.h"
#include "../../../include/dev_def.h"
#if DEV_ADC_ENABLE

#include "soc/soc.h"			/* APB_CLK_FREQ */
#include "esp_private/periph_ctrl.h"	/* PERIPH_RCC_ATOMIC */
#include "hal/adc_oneshot_hal.h"
#include "hal/adc_hal_common.h"
#include "hal/adc_types.h"
#include "hal/adc_ll.h"			/* _adc_ll_enable_bus_clock, adc_ll_enable_func_clock */

/* Per-unit HAL context, kept across opens/reads. */
LOCAL adc_oneshot_hal_ctx_t ll_hal[DEV_ADC_UNITNM];

LOCAL adc_unit_t unit_to_adc(UW unit)
{
	(void)unit;	/* DEV_ADC_UNITNM == 1; always ADC_UNIT_1 */
	return ADC_UNIT_1;
}

/*
 * One conversion at `channel`, blocking.  Returns raw 12-bit code.
 * adc_oneshot_hal_convert() handles enable/start/poll/disable internally.
 */
LOCAL ER adc_convert_one(UW unit, INT channel, UW *out)
{
	int raw = 0;
	adc_oneshot_hal_setup(&ll_hal[unit], (adc_channel_t)channel);
	if (!adc_oneshot_hal_convert(&ll_hal[unit], &raw)) {
		return E_IO;
	}
	*out = (UW)raw;
	return E_OK;
}

LOCAL W adc_read_block(UW unit, INT start, INT size, UW *buf)
{
	if (start < 0 || start >= ADC_CH_NUM) return (W)E_PAR;
	if (size <= 0 || (start + size) > ADC_CH_NUM) return (W)E_PAR;
	if (buf == NULL) return (W)E_PAR;

	for (INT i = 0; i < size; i++) {
		ER er = adc_convert_one(unit, start + i, &buf[i]);
		if (er != E_OK) return (W)er;
	}
	return (W)size;
}

/*----------------------------------------------------------------------
 * Low-level device control.
 */
EXPORT W dev_adc_llctl(UW unit, INT cmd, UW p1, UW p2, UW *pp)
{
	switch (cmd) {
	case LLD_ADC_OPEN:
	case LLD_ADC_CLOSE:
		/* Per-channel setup happens in convert; nothing to do on open/close. */
		return (W)E_OK;

	case LLD_ADC_READ:
		return adc_read_block(unit, (INT)p1, (INT)p2, pp);

	case LLD_ADC_RSIZE: {
		W rem = (W)ADC_CH_NUM - (W)p1;
		return (rem < 0) ? 0 : rem;
	}
	}
	return (W)E_PAR;
}

/*----------------------------------------------------------------------
 * Device initialization.  Powers on the ADC module, initializes the HAL
 * context, and pre-registers default attenuation/bitwidth for every
 * channel so reads just need a setup+convert pair.
 */
EXPORT ER dev_adc_llinit(T_ADC_DCB *p_dcb)
{
	UW unit = p_dcb->unit;

	/* The SARADC peripheral isn't in the C3's legacy periph_defs enum
	 * (it migrated to the RCC-atomic API), so enable the bus clock and
	 * deassert reset the new way before turning on the functional clock. */
	PERIPH_RCC_ATOMIC() {
		_adc_ll_enable_bus_clock(true);
		_adc_ll_reset_register();
	}
	adc_ll_enable_func_clock(true);

	adc_oneshot_hal_cfg_t cfg = {
		.unit            = unit_to_adc(unit),
		.work_mode       = ADC_HAL_SINGLE_READ_MODE,
		.clk_src         = ADC_DIGI_CLK_SRC_DEFAULT,
		.clk_src_freq_hz = APB_CLK_FREQ,
	};
	adc_oneshot_hal_init(&ll_hal[unit], &cfg);

	adc_oneshot_hal_chan_cfg_t chan_cfg = {
		.atten    = (adc_atten_t)DEVCNF_ADC_ATTEN,
		.bitwidth = (adc_bitwidth_t)DEVCNF_ADC_BITWIDTH,
	};
	for (INT ch = 0; ch < ADC_CH_NUM; ch++) {
		adc_oneshot_hal_channel_config(&ll_hal[unit], &chan_cfg, (adc_channel_t)ch);
	}

	return E_OK;
}

#endif	/* DEV_ADC_ENABLE */
#endif	/* CPU_ESP32C3 */
