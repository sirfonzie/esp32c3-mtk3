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
