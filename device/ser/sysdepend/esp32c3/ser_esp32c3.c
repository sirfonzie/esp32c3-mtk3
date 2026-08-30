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

/*
 *	ser_esp32c3.c
 *	Serial communication device driver
 *	System dependent processing for ESP32-C3 (UART1, external pin path)
 *
 *	UART0 is owned by the ROM-side tm_printf path; this driver only exposes
 *	UART1 via the device manager.  TX and RX are routed to real pins through
 *	the GPIO matrix (see setup_pins); the UART's internal loopback is left
 *	OFF, so a self-test needs a jumper between DEVCNF_SER_TX_PIN and
 *	DEVCNF_SER_RX_PIN (GPIO21 and GPIO20 by default).
 */

#include <sys/machine.h>
#ifdef CPU_ESP32C3

#include <tk/tkernel.h>
#include "../../ser.h"
#include "../../../include/dev_def.h"
#if DEV_SER_ENABLE

#include "hal/uart_ll.h"
#include "hal/gpio_ll.h"
#include "soc/periph_defs.h"
#include "soc/gpio_sig_map.h"
#include "soc/io_mux_reg.h"
#include "esp_private/periph_ctrl.h"
#include "esp_rom_gpio.h"

#define XTAL_CLK_FREQ_HZ	40000000U	/* ESP32-C3 XTAL is 40 MHz */

/*----------------------------------------------------------------------
 * Per-unit interrupt source (ETS_*_SOURCE -- this is the intno passed
 * to tk_def_int on this port).
 */
const LOCAL struct {
	UINT	intno;
	PRI	intpri;
} ll_devdat[DEV_SER_UNITNM] = {
	{ INTNO_UART1, DEVCNF_UART1_INTPRI },
};

LOCAL uart_dev_t *unit_hw(UW unit) { return UART_LL_GET_HW(unit + 1); }
LOCAL int unit_port(UW unit) { return (int)(unit + 1); }

/* Route UART1 TX/RX through the GPIO matrix to the configured pins.
 * Same call site as the IDF default uart_set_pin would use, just without
 * pulling in esp_driver_uart (which is FreeRTOS-tied). */
LOCAL void setup_pins(void)
{
	gpio_dev_t *gpio = GPIO_LL_GET_HW(0);

	/* TX: drive out of the matrix. */
	gpio_ll_func_sel(gpio, DEVCNF_SER_TX_PIN, PIN_FUNC_GPIO);
	gpio_ll_output_enable(gpio, DEVCNF_SER_TX_PIN);
	esp_rom_gpio_connect_out_signal(DEVCNF_SER_TX_PIN, U1TXD_OUT_IDX, false, false);

	/* RX: pull-up so the line idles high when nothing is wired, route into
	 * the matrix.  output_enable on the pad is intentionally left off. */
	gpio_ll_func_sel(gpio, DEVCNF_SER_RX_PIN, PIN_FUNC_GPIO);
	gpio_ll_input_enable(gpio, DEVCNF_SER_RX_PIN);
	gpio_ll_pullup_en(gpio, DEVCNF_SER_RX_PIN);
	esp_rom_gpio_connect_in_signal(DEVCNF_SER_RX_PIN, U1RXD_IN_IDX, false);
}

/*----------------------------------------------------------------------
 * Low-level control-block: shadow of mode/speed between MODE/SPEED
 * configuration calls and the START call that actually programs the HW.
 */
typedef struct {
	UW	mode;
	UW	speed;
} T_DEV_SER_LLDEVCB;

LOCAL T_DEV_SER_LLDEVCB ll_devcb[DEV_SER_UNITNM];

/* UART interrupt sources we care about: any byte arrival (FULL at threshold
 * 1) plus an idle-timeout safety net, the FIFO-empty event for paced TX, and
 * the line-error sources. */
#define RX_INTR_MASK	(UART_INTR_RXFIFO_FULL | UART_INTR_RXFIFO_TOUT)
#define ERR_INTR_MASK	(UART_INTR_PARITY_ERR | UART_INTR_FRAM_ERR | UART_INTR_RXFIFO_OVF)

/*----------------------------------------------------------------------
 * Interrupt handler.  Single source per unit -- both RX and TX events
 * arrive here.  Reads/dispatches every pending byte on the RX side, then
 * drains the kernel's send buffer into the TX FIFO until either runs out.
 */
void uart_inthdr(UINT intno)
{
	W unit;
	if (intno == INTNO_UART1) {
		unit = 0;
	} else {
		return;
	}

	uart_dev_t *hw = unit_hw(unit);
	UW sts = uart_ll_get_intsts_mask(hw);

	if (sts & RX_INTR_MASK) {
		while (uart_ll_get_rxfifo_len(hw) > 0) {
			uint8_t b;
			uart_ll_read_rxfifo(hw, &b, 1);
			dev_ser_notify_rcv((UW)unit, (UW)b);
		}
	}

	if (sts & UART_INTR_TXFIFO_EMPTY) {
		UW data;
		while (uart_ll_get_txfifo_len(hw) > 0 &&
		       dev_ser_get_snddat((UW)unit, &data)) {
			uint8_t b = (uint8_t)data;
			uart_ll_write_txfifo(hw, &b, 1);
		}
		/* No more user data queued -- mask the TX event so it doesn't
		 * re-fire continuously on an empty FIFO. */
		uart_ll_disable_intr_mask(hw, UART_INTR_TXFIFO_EMPTY);
	}

	if (sts & ERR_INTR_MASK) {
		UW err = 0;
		if (sts & UART_INTR_PARITY_ERR) err |= DEV_SER_ERR_PE;
		if (sts & UART_INTR_FRAM_ERR)   err |= DEV_SER_ERR_FE;
		if (sts & UART_INTR_RXFIFO_OVF) err |= DEV_SER_ERR_OE;
		dev_ser_notify_err((UW)unit, err);
	}

	uart_ll_clr_intsts_mask(hw, sts);
}

/*----------------------------------------------------------------------
 * Translate the user-facing mode word (see ser_mode_sysdep.h) into UART
 * register fields and load them.  Called from LLD_SER_START.
 */
LOCAL void start_com(UW unit, UW mode, UW speed)
{
	uart_dev_t *hw = unit_hw(unit);

	uart_word_length_t wl;
	switch (mode & DEV_SER_MODE_WL_MASK) {
	case DEV_SER_MODE_5BIT: wl = UART_DATA_5_BITS; break;
	case DEV_SER_MODE_6BIT: wl = UART_DATA_6_BITS; break;
	case DEV_SER_MODE_7BIT: wl = UART_DATA_7_BITS; break;
	default:                wl = UART_DATA_8_BITS; break;
	}
	uart_ll_set_data_bit_num(hw, wl);

	uart_ll_set_stop_bits(hw,
		(mode & DEV_SER_MODE_STOP_MASK) ? UART_STOP_BITS_2 : UART_STOP_BITS_1);

	uart_parity_t par;
	switch (mode & DEV_SER_MODE_P_MASK) {
	case DEV_SER_MODE_PODD:  par = UART_PARITY_ODD;  break;
	case DEV_SER_MODE_PEVEN: par = UART_PARITY_EVEN; break;
	default:                 par = UART_PARITY_DISABLE; break;
	}
	uart_ll_set_parity(hw, par);

	(void)uart_ll_set_baudrate(hw, speed, XTAL_CLK_FREQ_HZ);

	/* External pin path: TX/RX go through the GPIO matrix (set up in
	 * dev_ser_llinit via setup_pins).  Self-test needs a jumper between
	 * DEVCNF_SER_TX_PIN and DEVCNF_SER_RX_PIN. */
	uart_ll_set_loop_back(hw, false);

	uart_ll_rxfifo_rst(hw);
	uart_ll_txfifo_rst(hw);

	/* RX: fire on first byte; TX: only enabled while data is pending. */
	uart_ll_set_rxfifo_full_thr(hw, 1);
	uart_ll_set_txfifo_empty_thr(hw, 0);

	uart_ll_clr_intsts_mask(hw, UART_LL_INTR_MASK);
	uart_ll_ena_intr_mask(hw, RX_INTR_MASK | ERR_INTR_MASK);
}

LOCAL void stop_com(UW unit)
{
	uart_dev_t *hw = unit_hw(unit);
	uart_ll_disable_intr_mask(hw, UART_LL_INTR_MASK);
	uart_ll_clr_intsts_mask(hw, UART_LL_INTR_MASK);
	uart_ll_set_loop_back(hw, false);
}

/*----------------------------------------------------------------------
 * Low level device control -- called by ser.c.
 */
EXPORT ER dev_ser_llctl(UW unit, INT cmd, UW parm)
{
	uart_dev_t *hw = unit_hw(unit);
	ER err = E_OK;

	switch (cmd) {
	case LLD_SER_MODE:
		ll_devcb[unit].mode = parm;
		break;

	case LLD_SER_SPEED:
		ll_devcb[unit].speed = parm;
		break;

	case LLD_SER_START:
		start_com(unit, ll_devcb[unit].mode, ll_devcb[unit].speed);
		EnableInt(ll_devdat[unit].intno, ll_devdat[unit].intpri);
		break;

	case LLD_SER_STOP:
		DisableInt(ll_devdat[unit].intno);
		stop_com(unit);
		break;

	case LLD_SER_SEND:
		/* ser.c calls this on the first byte of a write burst (when the
		 * snd_buff was empty).  Push directly to the TX FIFO and arm the
		 * empty-interrupt so the ISR can pull the rest. */
		if (uart_ll_get_txfifo_len(hw) > 0) {
			uint8_t b = (uint8_t)parm;
			uart_ll_write_txfifo(hw, &b, 1);
			uart_ll_ena_intr_mask(hw, UART_INTR_TXFIFO_EMPTY);
		} else {
			err = E_BUSY;
		}
		break;

	case LLD_SER_BREAK:
		uart_ll_tx_break(hw, parm ? 1 : 0);
		break;
	}

	return err;
}

/*----------------------------------------------------------------------
 * Device initialization (called once at registration time by ser.c).
 */
EXPORT ER dev_ser_llinit(T_SER_DCB *p_dcb)
{
	const T_DINT dint = {
		.intatr	= TA_HLNG,
		.inthdr	= (FP)uart_inthdr,
	};
	UW unit = p_dcb->unit;
	uart_dev_t *hw = unit_hw(unit);
	ER err;

	periph_module_enable(PERIPH_UART1_MODULE + unit);

	uart_ll_set_sclk(hw, (soc_module_clk_t)UART_SCLK_XTAL);
	uart_ll_sclk_enable(hw);

	uart_ll_disable_intr_mask(hw, UART_LL_INTR_MASK);
	uart_ll_clr_intsts_mask(hw, UART_LL_INTR_MASK);

	setup_pins();

	p_dcb->intno_rcv = p_dcb->intno_snd = ll_devdat[unit].intno;
	p_dcb->int_pri   = ll_devdat[unit].intpri;

	err = tk_def_int(ll_devdat[unit].intno, &dint);
	(void)unit_port;
	return err;
}

#endif	/* DEV_SER_ENABLE */
#endif	/* CPU_ESP32C3 */
