/*
 *	ser_sysdep.h
 *	Serial communication device driver
 *	System-dependent definition for ESP32-C3
 *
 *	Single channel (UART1).  UART0 is owned by the ROM-side tm_printf path
 *	and is not exposed through the device manager.
 */

#ifndef __DEV_SER_ESP32C3_H__
#define __DEV_SER_ESP32C3_H__

#include "soc/interrupts.h"

#define DEV_SER_UNITNM	(1)		/* Number of device channels */
#define DEV_SER_UNIT0	(0)		/* Ch.0 - UART1 */

/* Maps a unit to its Espressif ETS_*_SOURCE enum (the intno passed to
 * tk_def_int on this port -- see include/tk/sysdepend/cpu/core/riscv_esp/syslib.h). */
#define INTNO_UART1	ETS_UART1_INTR_SOURCE

#endif		/* __DEV_SER_ESP32C3_H__ */
