/*
 * ble_task.c — µT-Kernel task wrapper for BLE init.
 *
 * Includes <tk/tkernel.h> only (no ESP-IDF BT headers).
 * The actual NimBLE init lives in ble_init.c to avoid the MSTATUS macro
 * clash between sysdef.h and riscv/encoding.h when both are included.
 */

#include <tk/tkernel.h>
#include <tm/tmonitor.h>

extern void ble_init_run(void);   /* ble_init.c */

static void ble_task_entry(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;
    ble_init_run();
    /* ble_init_run calls tk_ext_tsk(); execution does not reach here. */
}

void ble_controller_start(void)
{
    T_CTSK ctsk = {
        .exinf   = NULL,
        .tskatr  = TA_HLNG | TA_RNG0,
        .task    = (FP)ble_task_entry,
        .itskpri = 5,
        .stksz   = 4096,
    };
    ID tid = tk_cre_tsk(&ctsk);
    if (tid > 0) {
        tk_sta_tsk(tid, 0);
    } else {
        tm_printf((UB *)"[ble] tk_cre_tsk FAILED: %d\n", (int)tid);
    }
}
