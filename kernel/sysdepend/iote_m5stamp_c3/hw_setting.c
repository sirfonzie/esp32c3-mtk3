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
#ifdef IOTE_M5STAMP_C3

#include "kernel.h"
#include <tm/tmonitor.h>
#include "sysdepend.h"

#include "esp_system.h"

/*
 * ESP-IDF bootloader/startup has already initialized clocks, pin mux, and
 * boot-time peripherals before this port enters knl_main().
 */
EXPORT void knl_startup_hw(void) { }

#if USE_SHUTDOWN
EXPORT void knl_shutdown_hw(void)
{
	DISABLE_INTERRUPT;
	for (;;) { }
}
#endif

EXPORT ER knl_restart_hw(W mode)
{
	switch (mode) {
	case -1:
	case -3:
		esp_restart();		/* does not return */
		return E_OK;
	case -2:
		return E_NOSPT;
	default:
		return E_PAR;
	}
}

#endif
