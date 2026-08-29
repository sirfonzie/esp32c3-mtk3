/*
 * mtk_startup.c -- weak default startup for the ESP32-C3 port.
 *
 * Bypasses FreeRTOS by providing __wrap_esp_startup_start_app (paired with
 * the --wrap=esp_startup_start_app linker flag in this component), provisions
 * a 32 KB static heap for the kernel object pools, and hands off to
 * knl_main().  Apps that need a different heap size, a different startup
 * banner, or boot-time work before knl_main() runs can override this with a
 * strong definition of the same symbol in their own translation unit.
 */

#include <stdint.h>
#include <sys/machine.h>
#include "esp_rom_sys.h"

#ifdef IOTE_M5STAMP_C3
#include <stddef.h>
#include "hal/uart_ll.h"
#endif

extern int knl_main(void);
extern void *knl_lowmem_top;
extern void *knl_lowmem_limit;

#define MTK_DEFAULT_HEAP_BYTES	(32 * 1024)

static uint8_t mtk_default_heap[MTK_DEFAULT_HEAP_BYTES] __attribute__((aligned(8)));

#ifdef IOTE_M5STAMP_C3
static void mtk_startup_puts(const char *s)
{
	uart_dev_t *hw = UART_LL_GET_HW(0);
	for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; p++) {
		for (uint32_t retry = 0; uart_ll_get_txfifo_len(hw) == 0; retry++) {
			if (retry > 100000) return;
		}
		uart_ll_write_txfifo(hw, p, 1);
	}
}
#else
#define mtk_startup_puts(s)	esp_rom_printf("%s", (s))
#endif

__attribute__((weak))
void __wrap_esp_startup_start_app(void)
{
	mtk_startup_puts("\n[mtkernel] __wrap_esp_startup_start_app: bypassing FreeRTOS\n");

	knl_lowmem_top   = mtk_default_heap;
	knl_lowmem_limit = mtk_default_heap + MTK_DEFAULT_HEAP_BYTES;
#ifndef IOTE_M5STAMP_C3
	esp_rom_printf("[mtkernel] heap %p..%p (%u bytes)\n",
	               knl_lowmem_top, knl_lowmem_limit,
	               (unsigned)MTK_DEFAULT_HEAP_BYTES);
#else
	mtk_startup_puts("[mtkernel] heap configured (32768 bytes)\n");
#endif

	mtk_startup_puts("[mtkernel] calling knl_main()\n");
	(void)knl_main();

	mtk_startup_puts("[mtkernel] knl_main returned (unexpected), halting\n");
	for (;;) { }
}
