/*
 * smoke_kernel_api.c -- Smoke tests for the portable micro T-Kernel
 * syscalls that the rest of the demos don't exercise.
 *
 * Each test returns E_OK on success (a non-zero error code on failure)
 * and prints a PASS/FAIL line.  Sync-primitive tests use a short-lived
 * worker task that signals from the side; mempool/cyc/alm tests run
 * entirely from the initial task.
 *
 * Run from the initial task AFTER the idle task is created (so blocking
 * waits can't strand schedtsk = NULL -- see invariant #3 in PORT_ESP32C3.md).
 */

#include <tk/tkernel.h>
#include <tm/tmonitor.h>

#define WORKER_PRI	1	/* same as initial task; runs only when initial blocks */
#define WORKER_STK	1024

static int passed, failed;

static void report(const char *name, ER er, const char *detail)
{
	if (er == E_OK) {
		passed++;
		tm_printf((UB *)"[ktest] %-12s PASS%s%s\n", name,
		          detail ? " " : "", detail ? detail : "");
	} else {
		failed++;
		tm_printf((UB *)"[ktest] %-12s FAIL er=%d%s%s\n", name, er,
		          detail ? " " : "", detail ? detail : "");
	}
}

/* ------------------------------------------------------------------ */
/* Semaphore: worker signals after a delay, initial waits.            */
/* ------------------------------------------------------------------ */
static ID t_sem_id;

static void sem_worker(INT stacd, void *exinf)
{
	(void)stacd; (void)exinf;
	tk_dly_tsk(20);
	tk_sig_sem(t_sem_id, 1);
	tk_exd_tsk();
}

static ER test_semaphore(void)
{
	T_CSEM csem = { .sematr = TA_TFIFO, .isemcnt = 0, .maxsem = 1 };
	t_sem_id = tk_cre_sem(&csem);
	if (t_sem_id < 0) return (ER)t_sem_id;

	T_CTSK ctsk = { .tskatr = TA_HLNG | TA_RNG0, .task = (FP)sem_worker,
	                .itskpri = WORKER_PRI, .stksz = WORKER_STK };
	ID w = tk_cre_tsk(&ctsk);
	if (w < 0) { tk_del_sem(t_sem_id); return (ER)w; }
	tk_sta_tsk(w, 0);

	ER er = tk_wai_sem(t_sem_id, 1, 200);
	tk_del_sem(t_sem_id);
	return er;
}

/* ------------------------------------------------------------------ */
/* Eventflag: worker sets bit 0x1; initial waits AND on 0x1.          */
/* ------------------------------------------------------------------ */
static ID t_flg_id;

static void flg_worker(INT stacd, void *exinf)
{
	(void)stacd; (void)exinf;
	tk_dly_tsk(20);
	tk_set_flg(t_flg_id, 0x1);
	tk_exd_tsk();
}

static ER test_eventflag(void)
{
	T_CFLG cflg = { .flgatr = TA_TFIFO | TA_WMUL, .iflgptn = 0 };
	t_flg_id = tk_cre_flg(&cflg);
	if (t_flg_id < 0) return (ER)t_flg_id;

	T_CTSK ctsk = { .tskatr = TA_HLNG | TA_RNG0, .task = (FP)flg_worker,
	                .itskpri = WORKER_PRI, .stksz = WORKER_STK };
	ID w = tk_cre_tsk(&ctsk);
	if (w < 0) { tk_del_flg(t_flg_id); return (ER)w; }
	tk_sta_tsk(w, 0);

	UINT ptn = 0;
	ER er = tk_wai_flg(t_flg_id, 0x1, TWF_ANDW, &ptn, 200);
	tk_del_flg(t_flg_id);
	return er;
}

/* ------------------------------------------------------------------ */
/* Mutex: round-trip lock/unlock from the initial task.               */
/* (Contention scenarios with priority inversion belong in a deeper   */
/* test; the goal here is "the API works at all".)                    */
/* ------------------------------------------------------------------ */
static ER test_mutex(void)
{
	T_CMTX cmtx = { .mtxatr = TA_TFIFO, .ceilpri = 0 };
	ID mtx = tk_cre_mtx(&cmtx);
	if (mtx < 0) return (ER)mtx;

	ER er = tk_loc_mtx(mtx, TMO_POL);
	if (er == E_OK) er = tk_unl_mtx(mtx);

	tk_del_mtx(mtx);
	return er;
}

/* ------------------------------------------------------------------ */
/* Mailbox: worker sends a message; initial receives.                 */
/* ------------------------------------------------------------------ */
static ID t_mbx_id;

static T_MSG mbx_msg;	/* lives outside the worker stack -- mailbox keeps the pointer */

static void mbx_worker(INT stacd, void *exinf)
{
	(void)stacd; (void)exinf;
	tk_dly_tsk(20);
	tk_snd_mbx(t_mbx_id, &mbx_msg);
	tk_exd_tsk();
}

static ER test_mailbox(void)
{
	T_CMBX cmbx = { .mbxatr = TA_TFIFO | TA_MFIFO };
	t_mbx_id = tk_cre_mbx(&cmbx);
	if (t_mbx_id < 0) return (ER)t_mbx_id;

	T_CTSK ctsk = { .tskatr = TA_HLNG | TA_RNG0, .task = (FP)mbx_worker,
	                .itskpri = WORKER_PRI, .stksz = WORKER_STK };
	ID w = tk_cre_tsk(&ctsk);
	if (w < 0) { tk_del_mbx(t_mbx_id); return (ER)w; }
	tk_sta_tsk(w, 0);

	T_MSG *got = NULL;
	ER er = tk_rcv_mbx(t_mbx_id, &got, 200);
	tk_del_mbx(t_mbx_id);
	if (er == E_OK && got != &mbx_msg) return E_SYS;
	return er;
}

/* ------------------------------------------------------------------ */
/* Fixed-size memory pool: allocate all blocks, fail on empty, then   */
/* release and re-acquire.                                            */
/* ------------------------------------------------------------------ */
static ER test_mempool_fixed(void)
{
	T_CMPF cmpf = { .mpfatr = TA_TFIFO, .mpfcnt = 3, .blfsz = 64,
	                .bufptr = NULL };
	ID mpf = tk_cre_mpf(&cmpf);
	if (mpf < 0) return (ER)mpf;

	void *blk[3] = { NULL, NULL, NULL };
	ER er = E_OK;
	for (int i = 0; i < 3 && er == E_OK; i++) {
		er = tk_get_mpf(mpf, &blk[i], TMO_POL);
	}
	if (er == E_OK) {
		void *extra = NULL;
		ER over = tk_get_mpf(mpf, &extra, TMO_POL);
		if (over != E_TMOUT) er = E_SYS;	/* pool should be empty now */
	}
	if (er == E_OK) {
		tk_rel_mpf(mpf, blk[0]);
		void *re = NULL;
		er = tk_get_mpf(mpf, &re, TMO_POL);
	}

	tk_del_mpf(mpf);
	return er;
}

/* ------------------------------------------------------------------ */
/* Variable-size memory pool: a couple of allocs and releases.        */
/* ------------------------------------------------------------------ */
static ER test_mempool_variable(void)
{
	T_CMPL cmpl = { .mplatr = TA_TFIFO, .mplsz = 1024, .bufptr = NULL };
	ID mpl = tk_cre_mpl(&cmpl);
	if (mpl < 0) return (ER)mpl;

	void *a = NULL, *b = NULL;
	ER er = tk_get_mpl(mpl, 256, &a, TMO_POL);
	if (er == E_OK) er = tk_get_mpl(mpl, 256, &b, TMO_POL);
	if (er == E_OK && a == b) er = E_SYS;
	if (er == E_OK) er = tk_rel_mpl(mpl, a);
	if (er == E_OK) er = tk_rel_mpl(mpl, b);

	tk_del_mpl(mpl);
	return er;
}

/* ------------------------------------------------------------------ */
/* Cyclic handler: 10ms period, count fires across a 100ms window.    */
/* ------------------------------------------------------------------ */
static volatile UW cyc_count;
static void cyc_hdr(void *exinf) { (void)exinf; cyc_count++; }

static ER test_cyclic(void)
{
	T_CCYC ccyc = { .cycatr = TA_HLNG | TA_STA, .cychdr = (FP)cyc_hdr,
	                .cyctim = 10, .cycphs = 10 };
	cyc_count = 0;
	ID cyc = tk_cre_cyc(&ccyc);
	if (cyc < 0) return (ER)cyc;

	tk_dly_tsk(100);
	tk_stp_cyc(cyc);
	tk_del_cyc(cyc);

	/* expect ~10; accept 8..12 to absorb tk_dly_tsk's ~10ms slack */
	if (cyc_count < 8 || cyc_count > 12) return E_SYS;
	return E_OK;
}

/* ------------------------------------------------------------------ */
/* Alarm handler: fires once after 30ms.                              */
/* ------------------------------------------------------------------ */
static volatile UW alm_count;
static void alm_hdr(void *exinf) { (void)exinf; alm_count++; }

static ER test_alarm(void)
{
	T_CALM calm = { .almatr = TA_HLNG, .almhdr = (FP)alm_hdr };
	alm_count = 0;
	ID alm = tk_cre_alm(&calm);
	if (alm < 0) return (ER)alm;

	tk_sta_alm(alm, 30);
	tk_dly_tsk(100);
	tk_del_alm(alm);

	if (alm_count != 1) return E_SYS;
	return E_OK;
}

/* ------------------------------------------------------------------ */
EXPORT void run_kernel_api_smoke(void)
{
	tm_printf((UB *)"\n[ktest] starting kernel-API smoke tests\n");
	passed = failed = 0;

	report("sem",       test_semaphore(),        NULL);
	report("flg",       test_eventflag(),        NULL);
	report("mtx",       test_mutex(),            NULL);
	report("mbx",       test_mailbox(),          NULL);
	report("mpf",       test_mempool_fixed(),    NULL);
	report("mpl",       test_mempool_variable(), NULL);
	report("cyc",       test_cyclic(),           NULL);
	report("alm",       test_alarm(),            NULL);

	tm_printf((UB *)"[ktest] %d passed, %d failed\n", passed, failed);
}
