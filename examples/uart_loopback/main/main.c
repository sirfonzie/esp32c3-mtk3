/*
 * uart_loopback: TX/RX pattern demo on UART1.
 *
 * Each second: send 'A' or 'B' alternately, then drain whatever the
 * RX side buffered during the previous sleep and print it.
 *
 * Single-task design.  An earlier two-task attempt deadlocked: mSDI
 * (device/common/drvif/msdrvif.c) holds the per-device mutex across
 * the read syscall, so when an rx task blocks inside tk_srea_dev
 * waiting for data, any other task trying to tk_swri_dev on the same
 * device waits forever.  Looping send-then-poll-rx in one task
 * avoids the conflict entirely.
 *
 * With a TX<->RX jumper between DEVCNF_SER_TX_PIN and DEVCNF_SER_RX_PIN
 * (defaults: GPIO21 and GPIO20) every '[tx] sent X' is followed by an
 * '[rx] got 0x.. (X)' a moment later.  Without the jumper, only the
 * [tx] line appears -- handy single-glance check for wire continuity.
 *
 *   idf.py -C examples/uart_loopback build flash monitor
 */

#include <tk/tkernel.h>
#include <tm/tmonitor.h>

LOCAL void task_idle(INT stacd, void *exinf)
{
	(void)stacd; (void)exinf;
	for (;;) asm volatile ("wfi" ::: "memory");
}

EXPORT INT usermain(void)
{
	tm_printf((UB *)"\n[ser] UART1 1Hz A/B toggle + RX drain\n");

	T_CTSK ctsk = {
		.exinf = NULL, .tskatr = TA_HLNG | TA_RNG0,
		.itskpri = CNF_MAX_TSKPRI, .stksz = 1024,
		.task = (FP)task_idle,
	};
	tk_sta_tsk(tk_cre_tsk(&ctsk), 0);

	ID dd = tk_opn_dev((UB *)"sera", TD_UPDATE);
	if (dd < E_OK) {
		tm_printf((UB *)"[ser] tk_opn_dev(sera)=%d\n", (INT)dd);
		tk_slp_tsk(TMO_FEVR);
	}

	for (int n = 0; ; n++) {
		/* TX one byte */
		UB tx = (n & 1) ? 'B' : 'A';
		SZ asize;
		ER er = tk_swri_dev(dd, 0, &tx, 1, &asize);
		tm_printf((UB *)"[tx] sent '%c' (er=%d)\n", tx, er);

		tk_dly_tsk(1000);

		/* Non-blocking RX: size=0 returns the count of buffered bytes
		 * without sleeping, so we know how much to pull next. */
		SZ avail;
		tk_srea_dev(dd, 0, NULL, 0, &avail);
		if (avail > 0) {
			UB rx[16] = {0};
			SZ got = avail > (SZ)sizeof(rx) ? (SZ)sizeof(rx) : avail;
			tk_srea_dev(dd, 0, rx, got, &asize);
			for (SZ i = 0; i < asize; i++) {
				tm_printf((UB *)"[rx] got 0x%02x ('%c')\n",
				          (unsigned)rx[i], rx[i]);
			}
		}
	}

	/* unreachable */
	tk_cls_dev(dd, 0);
	tk_slp_tsk(TMO_FEVR);
	return 0;
}
