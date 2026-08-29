/*
 * app_multitask.c -- Multi-task data-exchange demo for µT-Kernel 3.0.
 *
 * Four tasks exchange data via a shared mailbox and demonstrate two
 * approaches to periodic scheduling with timestamp-validated drift:
 *
 *   task_producer_naive (pri 5, 2 s period)
 *       Uses a plain tk_dly_tsk(PERIOD) each cycle.  Drift from the ideal
 *       schedule accumulates by ~1 tick per cycle because the sleep starts
 *       from the END of the previous period, not from a fixed anchor.
 *
 *   task_producer_pll   (pri 5, 2 s period, 1 s phase offset)
 *       Phase-locked variant: computes the remaining time to the NEXT
 *       deadline and sleeps only that amount.  Any overshoot in one cycle
 *       is absorbed in the next, keeping long-term drift bounded by one
 *       tick granularity.
 *
 *   task_consumer (pri 6, event-driven)
 *       Blocks on tk_rcv_mbx, logs delivery latency per producer kind.
 *
 *   task_watchdog (pri 7, 5 s period)
 *       Prints a side-by-side summary for naive vs PLL every 5 s so the
 *       drift behaviour is easy to compare.
 *
 * Expected output contrast:
 *
 *   [prod-N] #10 @ 25621 ms  exp=25611  drift=+10 ms   ← grows each cycle
 *   [prod-P] #10 @ 26612 ms  exp=26612  drift= +0 ms   ← stays near zero
 *
 *   [wdog] naive: msgs=10  lat avg=0 max=0 ms  drift last=+10 max=10 ms
 *   [wdog] PLL  : msgs=10  lat avg=0 max=0 ms  drift last= +0 max= 1 ms
 *
 * Hardware-independent within the supported ESP32-C3 board kits.
 */

#include <tk/tkernel.h>
#include <tm/tmonitor.h>

#define PROD_PERIOD_MS   2000
#define PLL_PHASE_MS     1000   /* offset so naive/PLL prints interleave */
#define WDOG_PERIOD_MS   5000
#define TASK_STK         1536
#define MSG_POOL_SZ      4

/* ── Message type ─────────────────────────────────────────────────────────── */

#define KIND_NAIVE  0
#define KIND_PLL    1

typedef struct {
    T_MSG  hdr;            /* must be first for mailbox */
    UINT   seq;
    UW     t_send_ms;      /* SYSTIM.lo in ms at send time */
    UB     kind;           /* KIND_NAIVE or KIND_PLL */
} mtask_msg_t;

/* Separate pools so the two producers never alias each other's slots */
static mtask_msg_t pool_n[MSG_POOL_SZ];
static mtask_msg_t pool_p[MSG_POOL_SZ];

static ID s_mbx = -1;

/* Per-kind stats (index 0=naive, 1=PLL).
 * Single-core ESP32: plain volatile, no atomics needed. */
static volatile UINT s_count[2]      = {0, 0};
static volatile UW   s_max_lat[2]    = {0, 0};
static volatile UW   s_sum_lat[2]    = {0, 0};
static volatile UW   s_max_drift[2]  = {0, 0};
static volatile W    s_last_drift[2] = {0, 0};

/* ── Helpers ──────────────────────────────────────────────────────────────── */

static UW get_ms(void)
{
    SYSTIM t;
    tk_get_otm(&t);
    return t.lo;
}

static UW absu(W v) { return (UW)(v < 0 ? -v : v); }

/* ── Producers ────────────────────────────────────────────────────────────── */

/*
 * Naive producer: sleeps PERIOD_MS from end of each cycle.
 * Drift = actual_wake - ideal_schedule grows by ~1 ms per cycle.
 */
static void task_producer_naive(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;
    UW t_start = get_ms();
    UINT seq = 0;

    tm_printf((UB *)"[prod-N] started  naive  period=%d ms\n", PROD_PERIOD_MS);

    for (;;) {
        tk_dly_tsk(PROD_PERIOD_MS);
        seq++;

        UW t_now      = get_ms();
        UW t_expected = t_start + seq * (UW)PROD_PERIOD_MS;
        W  drift      = (W)(t_now - t_expected);

        if (absu(drift) > s_max_drift[KIND_NAIVE])
            s_max_drift[KIND_NAIVE] = absu(drift);
        s_last_drift[KIND_NAIVE] = drift;

        mtask_msg_t *m = &pool_n[seq % MSG_POOL_SZ];
        m->seq = seq;  m->t_send_ms = t_now;  m->kind = KIND_NAIVE;
        tk_snd_mbx(s_mbx, (T_MSG *)m);

        tm_printf((UB *)"[prod-N] #%u  @ %u ms  exp=%u  drift=%+d ms\n",
                  (unsigned)seq, (unsigned)t_now,
                  (unsigned)t_expected, (int)drift);
    }
}

/*
 * Phase-locked producer: computes remaining time to next deadline and sleeps
 * only that amount.  Overshoot in one cycle is absorbed in the next so
 * long-term drift is bounded to one tick granularity (~1 ms).
 */
static void task_producer_pll(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;
    /* Phase offset so prints interleave with the naive producer */
    tk_dly_tsk(PLL_PHASE_MS);

    UW t_start = get_ms();
    UINT seq = 0;

    tm_printf((UB *)"[prod-P] started  phase-locked  period=%d ms\n", PROD_PERIOD_MS);

    for (;;) {
        seq++;

        /* Sleep only the remaining time to the next deadline */
        UW t_deadline = t_start + seq * (UW)PROD_PERIOD_MS;
        UW t_now      = get_ms();
        W  remaining  = (W)(t_deadline - t_now);
        if (remaining > 1)
            tk_dly_tsk((RELTIM)remaining);

        t_now       = get_ms();
        W drift     = (W)(t_now - t_deadline);

        if (absu(drift) > s_max_drift[KIND_PLL])
            s_max_drift[KIND_PLL] = absu(drift);
        s_last_drift[KIND_PLL] = drift;

        mtask_msg_t *m = &pool_p[seq % MSG_POOL_SZ];
        m->seq = seq;  m->t_send_ms = t_now;  m->kind = KIND_PLL;
        tk_snd_mbx(s_mbx, (T_MSG *)m);

        tm_printf((UB *)"[prod-P] #%u  @ %u ms  exp=%u  drift=%+d ms\n",
                  (unsigned)seq, (unsigned)t_now,
                  (unsigned)t_deadline, (int)drift);
    }
}

/* ── Consumer ─────────────────────────────────────────────────────────────── */

static void task_consumer(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;
    tm_printf((UB *)"[cons] waiting on mailbox (handles both producers)\n");

    for (;;) {
        T_MSG *raw;
        if (tk_rcv_mbx(s_mbx, &raw, TMO_FEVR) != E_OK) continue;

        mtask_msg_t *m  = (mtask_msg_t *)raw;
        UW  t_recv      = get_ms();
        UW  lat         = t_recv - m->t_send_ms;
        UINT k          = m->kind;

        s_count[k]++;
        if (lat > s_max_lat[k]) s_max_lat[k] = lat;
        s_sum_lat[k] += lat;

        tm_printf((UB *)"[cons-%c] #%u  recv @ %u ms  lat=%u ms\n",
                  k ? 'P' : 'N',
                  (unsigned)m->seq, (unsigned)t_recv, (unsigned)lat);
    }
}

/* ── Watchdog ─────────────────────────────────────────────────────────────── */

static void task_watchdog(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;
    tm_printf((UB *)"[wdog] started  report every %d ms\n", WDOG_PERIOD_MS);

    for (;;) {
        tk_dly_tsk(WDOG_PERIOD_MS);
        for (int k = 0; k < 2; k++) {
            UINT n   = s_count[k];
            UW   avg = n ? s_sum_lat[k] / n : 0;
            tm_printf((UB *)"[wdog] %-5s  msgs=%u  lat avg=%u max=%u ms"
                             "  drift last=%+d max=%u ms\n",
                      k ? "PLL" : "naive",
                      (unsigned)n, (unsigned)avg, (unsigned)s_max_lat[k],
                      (int)s_last_drift[k], (unsigned)s_max_drift[k]);
        }
    }
}

/* ── Public API ───────────────────────────────────────────────────────────── */

void app_multitask_start(void)
{
    T_CMBX cmbx = { .mbxatr = TA_TFIFO | TA_MFIFO };
    s_mbx = tk_cre_mbx(&cmbx);
    if (s_mbx < E_OK) {
        tm_printf((UB *)"[mtask] tk_cre_mbx failed: %d\n", (int)s_mbx);
        return;
    }

    T_CTSK ct = { .tskatr = TA_HLNG | TA_RNG0, .stksz = TASK_STK };

    ct.itskpri = 5;  ct.task = (FP)task_producer_naive;
    ID tn = tk_cre_tsk(&ct);

    ct.itskpri = 5;  ct.task = (FP)task_producer_pll;
    ID tp = tk_cre_tsk(&ct);

    ct.itskpri = 6;  ct.task = (FP)task_consumer;
    ID tc = tk_cre_tsk(&ct);

    ct.itskpri = 7;  ct.task = (FP)task_watchdog;
    ID tw = tk_cre_tsk(&ct);

    tk_sta_tsk(tw, 0);
    tk_sta_tsk(tc, 0);
    tk_sta_tsk(tp, 0);
    tk_sta_tsk(tn, 0);

    tm_printf((UB *)"[mtask] naive=%d pll=%d consumer=%d watchdog=%d\n",
              (int)tn, (int)tp, (int)tc, (int)tw);
}
