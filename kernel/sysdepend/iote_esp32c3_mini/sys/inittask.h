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
 * ESP32-C3 override for sys/inittask.h — do not modify the upstream file.
 * #include_next pulls in the real sys/inittask.h from the standard include
 * path, then we override the two platform-specific values here.
 *
 * INITTASK_ITSKPRI 10: below ESP-IDF WiFi(2), sys_event(5), tcpip(7).
 * INITTASK_STKSZ  8KB: WiFi/BLE init chains overflow the upstream 1 KB.
 */
#include_next <sys/inittask.h>

#undef  INITTASK_ITSKPRI
#define INITTASK_ITSKPRI	(10)

#undef  INITTASK_STKSZ
#define INITTASK_STKSZ		(8*1024)
