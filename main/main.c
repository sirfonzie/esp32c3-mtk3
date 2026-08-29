#include <stdint.h>

#include "esp_rom_sys.h"
#include "esp_rom_serial_output.h"
#include "soc/interrupts.h"
#include "soc/system_reg.h"

#include <tk/tkernel.h>
#include <tm/tmonitor.h>

/* Startup glue (__wrap_esp_startup_start_app, default 32 KB kernel heap)
 * is supplied as a weak default by components/mtkernel/mtk_startup.c. */

/*
 * Lowest-priority idle task.  When all other tasks are blocked, the kernel
 * selects this one and the wrap swaps to it; the wfi parks the CPU until any
 * IRQ (typically the SYSTIMER tick that wakes another task) fires, at which
 * point the wrap swaps back out.  Without this, when ctxtsk goes to sleep
 * and no other task is ready, schedtsk = NULL and our wrap "returns" to the
 * sleeping task on its own stack -- which looks like tk_dly_tsk completing
 * instantly.  The idle task is the canonical fix.
 */
LOCAL void task_idle(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;
    for (;;) {
        asm volatile ("wfi" ::: "memory");
    }
}

/*
 * tk_def_int demo.  Registers an HLL handler on FROM_CPU_INTR2 and pings it
 * twice via the trigger register.  The source is level-triggered so the
 * handler clears the latch on entry to prevent continuous re-firing.
 */
static volatile UW test_isr_count = 0;

static void test_inthdr(UINT intno)
{
    (void)intno;
    WRITE_PERI_REG(SYSTEM_CPU_INTR_FROM_CPU_2_REG, 0);
    test_isr_count++;
}

static void run_def_int_smoke(void)
{
    T_DINT dint = { .intatr = TA_HLNG, .inthdr = (FP)test_inthdr };
    ER er = tk_def_int(ETS_FROM_CPU_INTR2_SOURCE, &dint);
    tm_printf((UB *)"[smoke] tk_def_int(INTR2)=%d\n", er);
    if (er != E_OK) return;
    EnableInt(ETS_FROM_CPU_INTR2_SOURCE, 1);

    UW before = test_isr_count;
    WRITE_PERI_REG(SYSTEM_CPU_INTR_FROM_CPU_2_REG, SYSTEM_CPU_INTR_FROM_CPU_2);
    tk_dly_tsk(20);
    WRITE_PERI_REG(SYSTEM_CPU_INTR_FROM_CPU_2_REG, SYSTEM_CPU_INTR_FROM_CPU_2);
    tk_dly_tsk(20);
    tm_printf((UB *)"[smoke] handler fired %u times (expect 2)\n",
              (unsigned)(test_isr_count - before));

    er = tk_def_int(ETS_FROM_CPU_INTR2_SOURCE, NULL);
    tm_printf((UB *)"[smoke] tk_def_int(INTR2, NULL)=%d\n", er);
}


/*
 * T-Monitor interactive RX demo.  Waits up to 5 s for a keypress; if one
 * arrives, reads a full line with tm_getline and echoes it back.  Skipped
 * automatically if no input is received so unattended runs are unaffected.
 */
static int wait_keypress_ms(int timeout_ms)
{
    int slept = 0;
    while (slept < timeout_ms) {
        uint8_t c;
        if (esp_rom_output_rx_one_char(&c) == 0) {
            return (int)c;
        }
        tk_dly_tsk(50);
        slept += 50;
    }
    return -1;
}

static void run_tmonitor_rx_demo(void)
{
    tm_printf((UB *)"[tm] press any key within 5s for interactive RX test...\n");
    int first = wait_keypress_ms(5000);
    if (first < 0) {
        tm_printf((UB *)"[tm] no input; skipping\n");
        return;
    }
    tm_printf((UB *)"[tm] got 0x%02x; now type a line + Enter:\n> ", first);

    UB line[64];
    tm_getline(line);
    tm_printf((UB *)"[tm] echoed: \"%s\"\n", line);
}

/*
 * Physical-timer demo.  ptmr1 runs as a free-running 1 MHz counter
 * (microsecond clock); ptmr2 fires a periodic handler at 10 kHz.  We
 * bracket the entire ptmr2 measurement window with ptmr1 reads so the
 * reported rate is computed against the actual elapsed microseconds
 * rather than the nominal tk_dly_tsk(100) -- which has up to one
 * kernel tick (10 ms) of slack.  Cross-checks both timers against each
 * other: if the rate isn't ~10000 Hz, either the 1 MHz clock or the
 * 100 us alarm period is mis-programmed.
 */
static volatile UW ptmr_tick_count = 0;
static void ptmr_tick_hdr(void *exinf) { (void)exinf; ptmr_tick_count++; }

static void run_ptimer_demo(void)
{
    T_RPTMR rp;
    if (GetPhysicalTimerConfig(1, &rp) == E_OK) {
        tm_printf((UB *)"[ptmr] clk=%u Hz max=%u\n",
                  (unsigned)rp.ptmrclk, (unsigned)rp.maxcount);
    }

    StartPhysicalTimer(1, 0xFFFFFFFFU, TA_CYC_PTMR);

    T_DPTMR dp = { .exinf = NULL, .ptmratr = 0, .ptmrhdr = (FP)ptmr_tick_hdr };
    DefinePhysicalTimerHandler(2, &dp);
    ptmr_tick_count = 0;
    StartPhysicalTimer(2, 100, TA_CYC_PTMR);

    UW t0, t1;
    GetPhysicalTimerCount(1, &t0);
    tk_dly_tsk(100);
    GetPhysicalTimerCount(1, &t1);
    UW count   = ptmr_tick_count;
    UW elapsed = t1 - t0;

    StopPhysicalTimer(2);
    StopPhysicalTimer(1);

    /* rate in Hz = count * 1e6 / elapsed_us; rearrange to avoid overflow */
    UW rate = (UW)(((unsigned long long)count * 1000000ULL) / elapsed);
    tm_printf((UB *)"[ptmr] window: %u us, fires: %u, measured rate: %u Hz (expect ~10000)\n",
              (unsigned)elapsed, (unsigned)count, (unsigned)rate);
}

/*
 * /adca demo.  Reads all 5 ADC1 channels (GPIO0..GPIO4) in one
 * tk_rea_dev call; pins are unwired on a bare board so values will be
 * noise floating around mid-scale or rail, but the call returning 5
 * raw codes in the 0..4095 range with no error confirms that the
 * driver, the polling-convert path, and the per-channel HAL setup all
 * work end-to-end.
 */
static void run_adc_demo(void)
{
    ID dd = tk_opn_dev((UB *)"adca", TD_READ);
    if (dd < E_OK) {
        tm_printf((UB *)"[adc] tk_opn_dev(adca)=%d\n", (INT)dd);
        return;
    }

    UW raw[5] = {0};
    SZ asize;
    ER er = tk_srea_dev(dd, 0, raw, 5, &asize);
    if (er == E_OK) {
        tm_printf((UB *)"[adc] ch0=%u ch1=%u ch2=%u ch3=%u ch4=%u (12-bit raw)\n",
                  (unsigned)raw[0], (unsigned)raw[1], (unsigned)raw[2],
                  (unsigned)raw[3], (unsigned)raw[4]);
    } else {
        tm_printf((UB *)"[adc] tk_srea_dev=%d asize=%d\n", er, (INT)asize);
    }

    tk_cls_dev(dd, 0);
}

EXPORT INT usermain(void)
{
    tm_printf((UB *)"\n[mtkernel] usermain on micro T-Kernel / ESP32-C3 (preemptive)\n");

    T_CTSK ctsk = {
        .exinf = NULL,
        .tskatr = TA_HLNG | TA_RNG0,
        .itskpri = 10,
        .stksz = 2048,
    };

    /* Idle task at lowest priority -- must exist before ANY task sleeps,
     * including the initial task's tk_dly_tsk inside run_def_int_smoke()
     * below.  Without it, schedtsk goes NULL and the wrap mret's to a stale
     * frame on the next IRQ -> instruction fault at PC=0. */
    ctsk.task = (FP)task_idle;
    ctsk.itskpri = CNF_MAX_TSKPRI;
    ctsk.stksz = 1024;
    ID idle = tk_cre_tsk(&ctsk);
    tk_sta_tsk(idle, 0);
    ctsk.itskpri = 10;
    ctsk.stksz = 2048;

    extern void run_kernel_api_smoke(void);
    run_kernel_api_smoke();

    run_def_int_smoke();
    run_ptimer_demo();
    run_adc_demo();
    run_tmonitor_rx_demo();

    extern void app_multitask_start(void);
    app_multitask_start();

#if CONFIG_MTK3_WIFI_ENABLED
    extern void wifi_init_run(void);
    wifi_init_run();
#endif
#if CONFIG_MTK3_BLE_ENABLED
    extern void ble_controller_start(void);
    ble_controller_start();
#endif

    tm_printf((UB *)"[mtkernel] demos started; initial task sleeping forever\n");
    tk_slp_tsk(TMO_FEVR);
    return 0;
}
