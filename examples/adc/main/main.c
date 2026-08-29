/*
 * adc: read all 5 ADC1 channels (GPIO0..GPIO4) in one device-manager
 * call.  Polling-based driver -- no IRQ needed since each conversion
 * is microseconds.  With pins floating the values will be noise around
 * mid-scale or pulled to a rail (GPIO2 on most C3 dev boards is the
 * chip-erase strapping pin, so ch2 commonly reads 4095).
 *
 *   idf.py -C examples/adc build flash monitor
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
	tm_printf((UB *)"\n[adc] ADC1 5-channel read, 1 Hz\n");

	T_CTSK ctsk = {
		.exinf = NULL, .tskatr = TA_HLNG | TA_RNG0,
		.itskpri = CNF_MAX_TSKPRI, .stksz = 1024,
		.task = (FP)task_idle,
	};
	tk_sta_tsk(tk_cre_tsk(&ctsk), 0);

	ID dd = tk_opn_dev((UB *)"adca", TD_READ);
	if (dd < E_OK) {
		tm_printf((UB *)"[adc] tk_opn_dev(adca)=%d\n", (INT)dd);
		tk_slp_tsk(TMO_FEVR);
	}

	for (int n = 0; ; n++) {
		UW raw[5] = {0};
		SZ asize;
		ER er = tk_srea_dev(dd, 0, raw, 5, &asize);
		if (er == E_OK) {
			tm_printf((UB *)"[adc %4d] ch0=%4u ch1=%4u ch2=%4u ch3=%4u ch4=%4u\n",
			          n,
			          (unsigned)raw[0], (unsigned)raw[1], (unsigned)raw[2],
			          (unsigned)raw[3], (unsigned)raw[4]);
		} else {
			tm_printf((UB *)"[adc %4d] tk_srea_dev=%d asize=%d\n",
			          n, er, (INT)asize);
		}
		tk_dly_tsk(1000);
	}

	/* unreachable */
	tk_cls_dev(dd, 0);
	tk_slp_tsk(TMO_FEVR);
	return 0;
}
