#include <sys/machine.h>
#ifdef IOTE_M5STAMP_C3

#include <sys/sysdef.h>
#include <tm/tmonitor.h>
#include <tk/tkernel.h>		/* pulls in config.h -> USE_SDEV_DRV before tk/device.h */
#include <tk/device.h>

#include "kernel.h"
#include "sysdepend.h"

/*
 * The ROM UART that backs tm_printf is owned directly by libtm/tm_com.c,
 * not by the micro T-Kernel device manager. UART1 is exposed as /ser0.
 */

EXPORT ER knl_init_device(void) { return E_OK; }

EXPORT ER knl_start_device(void)
{
#if USE_SDEV_DRV
	ER err;
	/*
	 * Do not register the shared ESP32-C3 DEV_SER instance on M5StampC3.
	 * That driver routes UART1 onto GPIO21/GPIO20, which are the UART0
	 * pins connected to the on-board CH9102 USB-UART bridge and must stay
	 * owned by the console.
	 */
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
