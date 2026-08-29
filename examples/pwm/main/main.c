/*
 * pwm: LEDC PWM demo on the ESP32-C3.
 *
 * Sets up LEDC channel 0 on timer 0, 1 kHz, 10-bit duty resolution
 * (0..1023), driven from the 80 MHz APB clock.  The channel output
 * is routed via the GPIO matrix to GPIO10.
 *
 * The loop walks the duty cycle through 0% / 25% / 50% / 75% / 100%
 * one step per second.  Wire an LED through ~470 ohms between GPIO10
 * and GND and you'll see the brightness step up and back down.  With
 * a multimeter set to AC RMS or DC average, you'll see the voltage
 * track the duty.
 *
 *   idf.py -C examples/pwm build flash monitor
 */

#include <stdint.h>
#include <stdbool.h>

#include <tk/tkernel.h>
#include <tm/tmonitor.h>

#include "esp_private/periph_ctrl.h"
#include "esp_rom_gpio.h"
#include "hal/gpio_ll.h"
#include "hal/ledc_ll.h"
#include "soc/gpio_sig_map.h"
#include "soc/io_mux_reg.h"

#define DEVCNF_PWM_PIN	10
#define APB_CLK_HZ	80000000U
#define PWM_FREQ_HZ	1000U
#define PWM_RES_BITS	10
#define PWM_MAX_DUTY	((1U << PWM_RES_BITS) - 1)	/* 1023 */

LOCAL void task_idle(INT s, void *e)
{
	(void)s; (void)e;
	for (;;) asm volatile ("wfi" ::: "memory");
}

/* Route LEDC low-speed channel 0 output to a GPIO pad.  GPIO matrix
 * signal index for channel N's PWM output is LEDC_LS_SIG_OUT<N>_IDX. */
LOCAL void pwm_route_pin(uint8_t pin)
{
	gpio_dev_t *gpio = GPIO_LL_GET_HW(0);
	gpio_ll_func_sel(gpio, pin, PIN_FUNC_GPIO);
	gpio_ll_output_enable(gpio, pin);
	gpio_ll_pulldown_dis(gpio, pin);
	gpio_ll_pullup_dis(gpio, pin);
	esp_rom_gpio_connect_out_signal(pin, LEDC_LS_SIG_OUT0_IDX, false, false);
}

LOCAL void pwm_init(void)
{
	ledc_dev_t *hw = LEDC_LL_GET_HW();

	/* Bus clock isn't in the C3 legacy periph_module_enable enum -- use
	 * the RCC-atomic helpers, same pattern as the ADC driver. */
	PERIPH_RCC_ATOMIC() {
		ledc_ll_enable_bus_clock(true);
		ledc_ll_enable_reset_reg(true);
		ledc_ll_enable_reset_reg(false);
	}
	ledc_ll_enable_mem_power(true);
	ledc_ll_enable_clock(hw, true);
	ledc_ll_enable_timer_power(hw, LEDC_LOW_SPEED_MODE, LEDC_TIMER_0, true);
	ledc_ll_enable_channel_power(hw, LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, true);

	/* APB / (divider/256 * 2^res) = freq.
	 *   divider_q18p8 = (APB << 8) / (freq * 2^res)
	 *   for 1 kHz @ 10-bit @ 80 MHz APB: (80e6 << 8) / (1000 * 1024) = 20000. */
	uint32_t div_q18p8 = ((uint64_t)APB_CLK_HZ << 8) / (PWM_FREQ_HZ * (1U << PWM_RES_BITS));

	ledc_ll_set_slow_clk_sel(hw, LEDC_SLOW_CLK_APB);
	ledc_ll_set_duty_resolution(hw, LEDC_LOW_SPEED_MODE, LEDC_TIMER_0, PWM_RES_BITS);
	ledc_ll_set_clock_divider(hw, LEDC_LOW_SPEED_MODE, LEDC_TIMER_0, div_q18p8);
	ledc_ll_timer_rst(hw, LEDC_LOW_SPEED_MODE, LEDC_TIMER_0);
	ledc_ll_ls_timer_update(hw, LEDC_LOW_SPEED_MODE, LEDC_TIMER_0);

	/* Channel: no fade, full duty written each update. */
	ledc_ll_set_hpoint(hw, LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
	ledc_ll_set_duty_num(hw, LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 1);
	ledc_ll_set_duty_cycle(hw, LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 1);
	ledc_ll_set_duty_scale(hw, LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
	ledc_ll_set_sig_out_en(hw, LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, true);
	ledc_ll_set_idle_level(hw, LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);

	pwm_route_pin(DEVCNF_PWM_PIN);
}

LOCAL void pwm_set_duty(uint32_t duty)
{
	ledc_dev_t *hw = LEDC_LL_GET_HW();
	/* LEDC duty register is the COMPARE point in (duty<<4) format. */
	ledc_ll_set_duty_int_part(hw, LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty << 4);
	ledc_ll_ls_channel_update(hw, LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

EXPORT INT usermain(void)
{
	tm_printf((UB *)"\n[pwm] LEDC ch0 on GPIO%d, %u Hz, %u-bit (max=%u)\n",
	          DEVCNF_PWM_PIN, (unsigned)PWM_FREQ_HZ,
	          (unsigned)PWM_RES_BITS, (unsigned)PWM_MAX_DUTY);

	T_CTSK ctsk = {
		.exinf = NULL, .tskatr = TA_HLNG | TA_RNG0,
		.itskpri = CNF_MAX_TSKPRI, .stksz = 1024,
		.task = (FP)task_idle,
	};
	tk_sta_tsk(tk_cre_tsk(&ctsk), 0);

	pwm_init();

	const uint32_t steps[] = { 0, PWM_MAX_DUTY/4, PWM_MAX_DUTY/2,
	                           (PWM_MAX_DUTY*3)/4, PWM_MAX_DUTY };
	const int n = sizeof(steps) / sizeof(steps[0]);

	for (int i = 0; ; i = (i + 1) % (2 * n - 2)) {
		int idx = (i < n) ? i : (2 * n - 2 - i);	/* ramp up then back */
		uint32_t duty = steps[idx];
		pwm_set_duty(duty);
		uint32_t pct = (duty * 100U + PWM_MAX_DUTY / 2) / PWM_MAX_DUTY;
		tm_printf((UB *)"[pwm] duty=%4u (%3u%%)\n", (unsigned)duty, (unsigned)pct);
		tk_dly_tsk(1000);
	}
}
