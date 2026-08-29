/*
 * four_tasks: 4 µT-Kernel tasks at different priorities and delays,
 * each printing its own incrementing counter.
 *
 *   A: pri=3 dly=1s   -- highest priority, fastest tick
 *   B: pri=4 dly=3s
 *   C: pri=5 dly=4s
 *   D: pri=6 dly=5s   -- lowest priority, slowest tick
 *
 * A prints once a second; D once every five.  Whenever A/B/C come
 * due, they preempt D mid-sleep, demonstrating the dispatcher
 * swapping stacks on schedule events.
 *
 *   idf.py -C examples/four_tasks build flash monitor
 */

#include <tk/tkernel.h>
#include <tm/tmonitor.h>

#include "hal/systimer_ll.h"
#include "soc/systimer_struct.h"

/* Independent wall-clock reference: read the free-running SYSTIMER
 * counter the kernel itself uses, but bypass knl_current_time so the
 * tick-accumulator and the raw counter can be compared.  C3 systimer
 * is fixed at 16 MHz, so /16000 = ms. */
static inline uint32_t hw_ms(void)
{
	systimer_dev_t *dev = &SYSTIMER;
	systimer_ll_counter_snapshot(dev, 0);
	while (!systimer_ll_is_counter_value_valid(dev, 0)) { }
	uint64_t v = ((uint64_t)systimer_ll_get_counter_value_high(dev, 0) << 32)
	           | systimer_ll_get_counter_value_low(dev, 0);
	return (uint32_t)(v / 16000ULL);
}

typedef struct {
	const char	*tag;
	int		pri;
	int		dly_ms;
} task_cfg_t;

LOCAL const task_cfg_t cfg_a = { "A", 3, 1000 };
LOCAL const task_cfg_t cfg_b = { "B", 4, 3000 };
LOCAL const task_cfg_t cfg_c = { "C", 5, 4000 };
LOCAL const task_cfg_t cfg_d = { "D", 6, 5000 };

LOCAL void task_idle(INT s, void *e)
{
	(void)s; (void)e;
	for (;;) asm volatile ("wfi" ::: "memory");
}

LOCAL void task_fn(INT s, void *e)
{
	(void)s;
	const task_cfg_t *cfg = (const task_cfg_t *)e;
	for (int i = 0; ; i++) {
		SYSTIM now = { 0, 0 };
		tk_get_otm(&now);
		tm_printf((UB *)"[t=%ums hw=%ums] [%s pri=%d dly=%ds] count=%d\n",
		          (unsigned)now.lo, (unsigned)hw_ms(),
		          (const UB *)cfg->tag, cfg->pri,
		          cfg->dly_ms / 1000, i);
		tk_dly_tsk(cfg->dly_ms);
	}
}

LOCAL ID spawn(const task_cfg_t *cfg)
{
	T_CTSK ctsk = {
		.exinf  = (void *)cfg,
		.tskatr = TA_HLNG | TA_RNG0,
		.task   = (FP)task_fn,
		.itskpri = cfg->pri,
		.stksz  = 1024,
	};
	ID t = tk_cre_tsk(&ctsk);
	tk_sta_tsk(t, 0);
	return t;
}

EXPORT INT usermain(void)
{
	T_CTSK idle = {
		.tskatr = TA_HLNG | TA_RNG0,
		.task = (FP)task_idle,
		.itskpri = CNF_MAX_TSKPRI,
		.stksz = 1024,
	};
	tk_sta_tsk(tk_cre_tsk(&idle), 0);

	tm_printf((UB *)"\n[four_tasks] spawning A/B/C/D...\n");
	spawn(&cfg_a);
	spawn(&cfg_b);
	spawn(&cfg_c);
	spawn(&cfg_d);

	tk_slp_tsk(TMO_FEVR);
	return 0;
}
