/*
 * tasks: minimal preemptive multitasking demo.
 *
 * Two µT-Kernel tasks at the same priority, phase-shifted by 500 ms, alternate
 * a "tick" message every second.  If the timestamps interleave cleanly, the
 * kernel's SYSTIMER tick is preempting tasks and the dispatcher is swapping
 * stacks as expected.
 *
 * Build & run:
 *   idf.py -C examples/tasks set-target esp32c3
 *   idf.py -C examples/tasks build flash monitor
 */

#include <tk/tkernel.h>
#include <tm/tmonitor.h>

LOCAL void task_idle(INT stacd, void *exinf)
{
	(void)stacd; (void)exinf;
	for (;;) {
		asm volatile ("wfi" ::: "memory");
	}
}

LOCAL void task_a(INT stacd, void *exinf)
{
	(void)stacd; (void)exinf;
	for (int i = 0; ; i++) {
		tm_printf((UB *)"[A] tick %d\n", i);
		tk_dly_tsk(1000);
	}
}

LOCAL void task_b(INT stacd, void *exinf)
{
	(void)stacd; (void)exinf;
	tk_dly_tsk(500);	/* phase-shift so B prints between A's */
	for (int i = 0; ; i++) {
		tm_printf((UB *)"[B] tick %d\n", i);
		tk_dly_tsk(1000);
	}
}

EXPORT INT usermain(void)
{
	tm_printf((UB *)"\n[tasks] preemptive multitasking demo\n");

	T_CTSK ctsk = {
		.exinf = NULL,
		.tskatr = TA_HLNG | TA_RNG0,
		.itskpri = 10,
		.stksz = 2048,
	};

	/* Idle task at lowest priority -- required so schedtsk never goes NULL
	 * when A/B are both sleeping (see invariant #3 in PORT_ESP32C3.md). */
	ctsk.task = (FP)task_idle;
	ctsk.itskpri = CNF_MAX_TSKPRI;
	ctsk.stksz = 1024;
	tk_sta_tsk(tk_cre_tsk(&ctsk), 0);

	ctsk.itskpri = 10;
	ctsk.stksz = 2048;
	ctsk.task = (FP)task_a;
	tk_sta_tsk(tk_cre_tsk(&ctsk), 0);
	ctsk.task = (FP)task_b;
	tk_sta_tsk(tk_cre_tsk(&ctsk), 0);

	tk_slp_tsk(TMO_FEVR);
	return 0;
}
