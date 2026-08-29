#include <sys/machine.h>
#ifdef IOTE_ESP32C3_MINI

#include "kernel.h"
#include <tm/tmonitor.h>
#include "sysdepend.h"

#include "esp_system.h"

/*
 * On Cortex-M ports knl_startup_hw() runs out of the reset handler and brings
 * up clocks, pin mux, peripheral clocks, etc.  On ESP32-C3 the IDF bootloader
 * has already done all of that before app_main is called, so this is a no-op.
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
