/*
 * usermain.c — µT-Kernel entry point for ble_gatt example
 *
 * Creates two tasks:
 *   - idle_task : lowest priority, sleeps the CPU with WFI
 *   - gatt_task : calls ble_gatt_run() from ble_nimble component
 *
 * Build:  idf.py -C examples/ble_gatt set-target esp32c3 &&
 *         idf.py -C examples/ble_gatt build
 * Flash:  idf.py -C examples/ble_gatt -p /dev/ttyACM0 flash monitor
 *
 * Verify with nRF Connect:
 *   1. Scanner → find "MTK3-GATT" → Connect
 *   2. Client → Unknown Service (0xFF00) → Read 0xFF01 (heap + uptime)
 *   3. Subscribe 0xFF01 for 3 s notifications
 *   4. Write a string to 0xFF02, then Read it back (echo)
 *   5. Device Information (0x180A) → read firmware/hardware strings
 */

#include <tk/tkernel.h>
#include <tm/tmonitor.h>

extern void ble_gatt_run(void);

LOCAL void idle_task(INT s, void *e)
{
    (void)s; (void)e;
    for (;;) Asm("wfi" ::: "memory");
}

LOCAL void gatt_task(INT s, void *e)
{
    (void)s; (void)e;
    ble_gatt_run();     /* never returns */
}

EXPORT INT usermain(void)
{
    tm_printf((UB *)"[gatt] MTK3 BLE GATT server\n");

    T_CTSK idle = {
        .tskatr  = TA_HLNG | TA_RNG0,
        .task    = (FP)idle_task,
        .itskpri = CNF_MAX_TSKPRI,
        .stksz   = 256,
    };
    tk_sta_tsk(tk_cre_tsk(&idle), 0);

    T_CTSK ctsk = {
        .tskatr  = TA_HLNG | TA_RNG0,
        .task    = (FP)gatt_task,
        .itskpri = 6,
        .stksz   = 8192,
    };
    tk_sta_tsk(tk_cre_tsk(&ctsk), 0);

    tk_slp_tsk(TMO_FEVR);
    return 0;
}
