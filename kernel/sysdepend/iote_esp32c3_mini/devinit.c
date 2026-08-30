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
#ifdef IOTE_ESP32C3_MINI

#include <sys/sysdef.h>
#include <tm/tmonitor.h>
#include <tk/tkernel.h>		/* pulls in config.h -> USE_SDEV_DRV before tk/device.h */
#include <tk/device.h>

#include "kernel.h"
#include "sysdepend.h"

/*
 * The ROM UART that backs tm_printf is owned directly by libtm/tm_com.c,
 * not by the micro T-Kernel device manager.  UART1 is exposed as /ser0.
 */

EXPORT ER knl_init_device(void) { return E_OK; }

EXPORT ER knl_start_device(void)
{
#if USE_SDEV_DRV
	ER err;
#if DEVCNF_USE_SER
	err = dev_init_ser(0);
	if (err < E_OK) return err;
#endif
#if DEVCNF_USE_ADC
	err = dev_init_adc(0);
	if (err < E_OK) return err;
#endif
#endif
	return E_OK;
}

#if USE_SHUTDOWN
EXPORT ER knl_finish_device(void) { return E_OK; }
#endif

#endif
