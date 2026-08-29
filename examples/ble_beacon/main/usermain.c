/*
 * usermain.c — µT-Kernel entry point for ble_beacon example
 *
 * Creates two tasks:
 *   - idle_task   : lowest priority, sleeps the CPU with WFI
 *   - beacon_task : calls ble_beacon_run() from ble_nimble component
 *
 * Build:  idf.py -C examples/ble_beacon set-target esp32c3 &&
 *         idf.py -C examples/ble_beacon build
 * Flash:  idf.py -C examples/ble_beacon -p /dev/ttyACM0 flash monitor
 *
 * Verify: nRF Connect → Scanner → "MTK3-Beacon" visible with manufacturer data
 */

#include <tk/tkernel.h>
#include <tm/tmonitor.h>

extern void ble_beacon_run(void);

LOCAL void idle_task(INT s, void *e)
{
    (void)s; (void)e;
    for (;;) Asm("wfi" ::: "memory");
}

LOCAL void beacon_task(INT s, void *e)
{
    (void)s; (void)e;
    ble_beacon_run();   /* never returns */
}

EXPORT INT usermain(void)
{
    tm_printf((UB *)"[beacon] MTK3 BLE non-connectable beacon\n");

    T_CTSK idle = {
        .tskatr  = TA_HLNG | TA_RNG0,
        .task    = (FP)idle_task,
        .itskpri = CNF_MAX_TSKPRI,
        .stksz   = 256,
    };
    tk_sta_tsk(tk_cre_tsk(&idle), 0);

    T_CTSK ctsk = {
        .tskatr  = TA_HLNG | TA_RNG0,
        .task    = (FP)beacon_task,
        .itskpri = 6,
        .stksz   = 6144,
    };
    tk_sta_tsk(tk_cre_tsk(&ctsk), 0);

    tk_slp_tsk(TMO_FEVR);
    return 0;
}
