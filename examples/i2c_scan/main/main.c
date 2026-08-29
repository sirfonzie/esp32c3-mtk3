/*
 * i2c_scan: walk the 7-bit I²C address range (0x08..0x77) and report
 * which addresses ACK.  Goes straight at the I²C MASTER controller via
 * hal/i2c_ll.h; no device-manager wrapping -- the scan IS the demo.
 *
 * Hardware: SDA on DEVCNF_SDA_PIN (GPIO6), SCL on DEVCNF_SCL_PIN
 * (GPIO7), with pull-ups (most sensor breakouts supply their own;
 * internal weak pull-ups are also enabled as a fallback for 100 kHz).
 *
 *   idf.py -C examples/i2c_scan build flash monitor
 */

#include <stdint.h>
#include <stdbool.h>

#include <tk/tkernel.h>
#include <tm/tmonitor.h>

#include "esp_private/periph_ctrl.h"
#include "esp_rom_gpio.h"
#include "hal/gpio_ll.h"
#include "hal/i2c_ll.h"
#include "soc/clk_tree_defs.h"
#include "soc/gpio_sig_map.h"
#include "soc/io_mux_reg.h"

#define DEVCNF_SDA_PIN	6
#define DEVCNF_SCL_PIN	7
#define DEVCNF_I2C_FREQ	100000U	/* 100 kHz standard-mode */
#define XTAL_FREQ_HZ	40000000U

LOCAL void task_idle(INT s, void *e)
{
	(void)s; (void)e;
	for (;;) asm volatile ("wfi" ::: "memory");
}

/* Wire SDA/SCL through the GPIO matrix as open-drain bidirectional pins
 * with weak internal pull-ups.  The matrix re-routes both the controller's
 * output signal AND the input side back into the controller so it can read
 * the line during ACK / arbitration. */
LOCAL void i2c_setup_pin(uint8_t pin, int out_sig, int in_sig)
{
	gpio_dev_t *gpio = GPIO_LL_GET_HW(0);
	gpio_ll_func_sel(gpio, pin, PIN_FUNC_GPIO);
	gpio_ll_input_enable(gpio, pin);
	gpio_ll_output_enable(gpio, pin);
	gpio_ll_od_enable(gpio, pin);
	gpio_ll_pullup_en(gpio, pin);
	esp_rom_gpio_connect_out_signal(pin, out_sig, false, false);
	esp_rom_gpio_connect_in_signal(pin, in_sig, false);
}

LOCAL void i2c_controller_init(void)
{
	i2c_dev_t *hw = I2C_LL_GET_HW(0);

	PERIPH_RCC_ATOMIC() {
		i2c_ll_enable_bus_clock(0, true);
		i2c_ll_reset_register(0);
	}

	i2c_ll_enable_controller_clock(hw, true);
	i2c_ll_set_source_clk(hw, (i2c_clock_source_t)I2C_CLK_SRC_DEFAULT);

	i2c_hal_clk_config_t clk_cfg = {0};
	i2c_ll_master_cal_bus_clk(XTAL_FREQ_HZ, DEVCNF_I2C_FREQ, &clk_cfg);
	i2c_ll_master_set_bus_timing(hw, &clk_cfg);

	i2c_ll_set_mode(hw, I2C_BUS_MODE_MASTER);
	i2c_ll_enable_pins_open_drain(hw, true);
	i2c_ll_master_set_filter(hw, 7);	/* glitch filter, ~7 ns @ XTAL */
	i2c_ll_enable_fifo_mode(hw, true);
	i2c_ll_disable_intr_mask(hw, UINT32_MAX);
	i2c_ll_clear_intr_mask(hw, UINT32_MAX);

	i2c_setup_pin(DEVCNF_SDA_PIN, I2CEXT0_SDA_OUT_IDX, I2CEXT0_SDA_IN_IDX);
	i2c_setup_pin(DEVCNF_SCL_PIN, I2CEXT0_SCL_OUT_IDX, I2CEXT0_SCL_IN_IDX);

	i2c_ll_update(hw);
}

/* One probe at `addr`: START + write(addr|W) + STOP.  Returns true if any
 * slave pulled SDA low during the ACK slot (MST_COMPLETE without NACK). */
LOCAL bool i2c_probe(uint8_t addr)
{
	i2c_dev_t *hw = I2C_LL_GET_HW(0);

	i2c_ll_txfifo_rst(hw);
	i2c_ll_rxfifo_rst(hw);
	i2c_ll_clear_intr_mask(hw, UINT32_MAX);

	i2c_ll_hw_cmd_t cmd_start = { .op_code = I2C_LL_CMD_RESTART };
	i2c_ll_hw_cmd_t cmd_write = { .op_code = I2C_LL_CMD_WRITE,
	                              .byte_num = 1, .ack_en = 1 };
	i2c_ll_hw_cmd_t cmd_stop  = { .op_code = I2C_LL_CMD_STOP };
	i2c_ll_master_write_cmd_reg(hw, cmd_start, 0);
	i2c_ll_master_write_cmd_reg(hw, cmd_write, 1);
	i2c_ll_master_write_cmd_reg(hw, cmd_stop,  2);

	uint8_t addr_byte = (uint8_t)((addr << 1) | 0);	/* write bit */
	i2c_ll_write_txfifo(hw, &addr_byte, 1);

	i2c_ll_update(hw);
	i2c_ll_start_trans(hw);

	/* Poll, ~5 ms budget.  At 100 kHz an address phase is ~90 us; the
	 * generous timeout covers stretched clocks or stuck bus. */
	uint32_t intr = 0;
	for (int i = 0; i < 5000; i++) {
		i2c_ll_get_intr_mask(hw, &intr);
		if (intr & (I2C_LL_INTR_MST_COMPLETE |
		            I2C_LL_INTR_NACK |
		            I2C_LL_INTR_TIMEOUT |
		            I2C_LL_INTR_ARBITRATION)) break;
		for (volatile int d = 0; d < 16; d++) ;	/* ~1 us at 160 MHz */
	}

	bool ack = (intr & I2C_LL_INTR_MST_COMPLETE) &&
	           !(intr & I2C_LL_INTR_NACK) &&
	           !(intr & I2C_LL_INTR_TIMEOUT) &&
	           !(intr & I2C_LL_INTR_ARBITRATION);

	/* Always reset the FSM after a probe so a NACK/timeout on one address
	 * doesn't bleed into the next. */
	i2c_ll_master_fsm_rst(hw);
	return ack;
}

LOCAL void i2c_scan(void)
{
	tm_printf((UB *)"\n     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n");
	int found = 0;
	for (uint8_t row = 0; row < 8; row++) {
		tm_printf((UB *)"%02x:", row * 16);
		for (uint8_t col = 0; col < 16; col++) {
			uint8_t addr = row * 16 + col;
			if (addr < 0x08 || addr > 0x77) {
				tm_printf((UB *)"   ");
				continue;
			}
			if (i2c_probe(addr)) {
				tm_printf((UB *)" %02x", addr);
				found++;
			} else {
				tm_printf((UB *)" --");
			}
		}
		tm_printf((UB *)"\n");
	}
	tm_printf((UB *)"\n[i2c] %d device(s) responded\n", found);
}

EXPORT INT usermain(void)
{
	tm_printf((UB *)"\n[i2c] address scanner on SDA=GPIO%d SCL=GPIO%d @ %u Hz\n",
	          DEVCNF_SDA_PIN, DEVCNF_SCL_PIN, (unsigned)DEVCNF_I2C_FREQ);

	T_CTSK ctsk = {
		.exinf = NULL, .tskatr = TA_HLNG | TA_RNG0,
		.itskpri = CNF_MAX_TSKPRI, .stksz = 1024,
		.task = (FP)task_idle,
	};
	tk_sta_tsk(tk_cre_tsk(&ctsk), 0);

	i2c_controller_init();
	i2c_scan();

	tk_slp_tsk(TMO_FEVR);
	return 0;
}
