/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.00.07.B0
 *
 *    Copyright (C) 2006-2023 by Ken Sakamura.
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 *
 *    Released by TRON Forum(http://www.tron.org) at 2023/11.
 *
 *----------------------------------------------------------------------
 */

#include <tk/tkernel.h>

#if USE_TMONITOR
#include "../../libtm.h"

#ifdef IOTE_M5STAMP_C3

#include "hal/uart_ll.h"

LOCAL uart_dev_t *tm_uart(void)
{
	return UART_LL_GET_HW(0);
}

LOCAL void tm_uart_putc(UB c)
{
	uart_dev_t *hw = tm_uart();
	for (UW retry = 0; uart_ll_get_txfifo_len(hw) == 0; retry++) {
		if (retry > 100000) return;
	}
	uart_ll_write_txfifo(hw, &c, 1);
}

LOCAL UB tm_uart_getc(void)
{
	uart_dev_t *hw = tm_uart();
	UB c;
	while (uart_ll_get_rxfifo_len(hw) == 0) { }
	uart_ll_read_rxfifo(hw, &c, 1);
	return c;
}

EXPORT void tm_snd_dat(const UB *buf, INT size)
{
	for (INT i = 0; i < size; i++) {
		tm_uart_putc(buf[i]);
	}
}

EXPORT void tm_rcv_dat(UB *buf, INT size)
{
	for (INT i = 0; i < size; i++) {
		buf[i] = tm_uart_getc();
	}
}

EXPORT void tm_com_init(void)
{
	/*
	 * M5StampC3 USB-C is attached through the on-board CH9102 USB-UART
	 * bridge to ESP32-C3 UART0. Select the UART console in sdkconfig.
	 */
}

#endif
#endif
