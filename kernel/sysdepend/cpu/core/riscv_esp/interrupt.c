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

#include <sys/machine.h>
#ifdef CPU_CORE_RISCV_ESP

#include "kernel.h"
#include "../../../sysdepend.h"

#include "esp_intr_alloc.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_rom_sys.h"
#include "soc/interrupts.h"
#include "hal/crosscore_int_ll.h"

#include "sdkconfig.h"
#if CONFIG_ESP_INT_WDT
/*
 * Interrupt-watchdog integration.  IDF normally arms the INT-WDT (TG MWDT
 * routed to the reserved panic vector) inside esp_startup_start_app() and
 * feeds it from the FreeRTOS tick hook.  This port wraps that function away,
 * so without the calls below the watchdog silently never runs even though
 * sdkconfig says ESP_INT_WDT=y.  Arming happens in knl_init_interrupt();
 * feeding reuses IDF's own tick-hook chain (esp_int_wdt_cpu_init registered
 * int_wdt.c's tick_hook there) via esp_vApplicationTickHook() from
 * knl_systim_inthdr() each kernel tick.
 *
 * Coverage note: kernel critical sections clear mstatus.MIE, which also
 * masks the WDT_STAGE0 panic interrupt -- a wedge with MIE held off gives no
 * panic backtrace.  WDT_STAGE1 (2x timeout) is a hardware system reset and
 * fires regardless of CPU state, so the device always recovers.
 */
#include "esp_private/esp_int_wdt.h"
/* Defined in esp_system/freertos_hooks.c; IDF declares it only at its own
 * FreeRTOS call site (port_systick.c), so declare it the same way here. */
extern void esp_vApplicationTickHook(void);
#endif

static intr_handle_t dispatch_intr_handle = NULL;

/*
 * Per-source registration table for tk_def_int().  Indexed by the user-supplied
 * intno, which on this port is an ETS_*_SOURCE enum value (see syslib.h).
 * N_INTVEC == ETS_MAX_INTR_SOURCE on ESP32-C3, so the table covers every
 * peripheral source the interrupt matrix can route.
 */
typedef struct {
	FP		hdr;		/* user handler, or NULL if unregistered */
	ATR		atr;		/* TA_HLNG | TA_ASM */
	intr_handle_t	handle;		/* IDF handle, freed on reset/redefine */
	INT		level;		/* ESP interrupt level used for handle */
} knl_user_int_t;

static knl_user_int_t knl_user_int_tbl[N_INTVEC];

#include "soc/soc.h"
#include "soc/interrupt_reg.h"	/* raw per-source status regs for CheckInt */
#if defined(CONFIG_IDF_TARGET_ESP32C3)
  #include "soc/system_reg.h"
  #define DISPATCH_INT_REG SYSTEM_CPU_INTR_FROM_CPU_0_REG
#elif defined(CONFIG_IDF_TARGET_ESP32C6)
  #include "soc/intpri_reg.h"
  #define DISPATCH_INT_REG INTPRI_CPU_INTR_FROM_CPU_0_REG
#endif

/*
 * Dispatcher software-interrupt ISR.  Triggered by knl_dispatch() writing
 * the FROM_CPU_INTR0 register.  The actual sp swap happens in
 * __wrap_rtos_int_exit (cpu_cntl.c) on the way out -- this handler only
 * clears the cause so the interrupt does not keep re-firing.
 */
static void IRAM_ATTR dispatch_isr(void *arg)
{
	(void)arg;
	crosscore_int_ll_clear_interrupt(0);
#ifdef DISPATCH_INT_REG
	(void)REG_READ(DISPATCH_INT_REG); /* Read back to flush the write buffer */
#endif
}

/*
 * Interrupt-subsystem init.  IDF's trap entry (components/riscv/vectors.S)
 * is installed via mtvec by the bootloader; we register only the cross-core
 * software IRQ that drives our dispatcher.  Peripheral interrupts are wired
 * with esp_intr_alloc() at their call sites (e.g. SYSTIMER in sys_timer.c).
 */
EXPORT ER knl_init_interrupt(void)
{
	esp_err_t err;

	/* No ESP_INTR_FLAG_IRAM: dispatch_isr calls kernel code in flash.
	 * The IDF SPI flash driver auto-masks non-IRAM interrupts during flash
	 * operations; context switches simply don't happen while flash writes. */
	err = esp_intr_alloc(ETS_FROM_CPU_INTR0_SOURCE,
	                     ESP_INTR_FLAG_LEVEL1,
	                     dispatch_isr, NULL, &dispatch_intr_handle);
	if (err != ESP_OK) {
		dispatch_intr_handle = NULL;
		return E_SYS;
	}

#if CONFIG_ESP_INT_WDT
	/* Arm the interrupt watchdog (see block comment at top of file).
	 * Initial stage timeouts are 5 s; the first tick's feed re-arms them
	 * to CONFIG_ESP_INT_WDT_TIMEOUT_MS.  The kernel timer starts a few ms
	 * after this point (knl_timer_startup in sysinit), well inside 5 s. */
	esp_int_wdt_init();
	esp_int_wdt_cpu_init();
#endif
	return E_OK;
}

/*
 * Trampoline invoked by the IDF interrupt dispatcher.  `arg` carries the
 * originating intno so a single C function services every source.  Wraps the
 * user handler with ENTER/LEAVE_TASK_INDEPENDENT per the micro T-Kernel ISR
 * contract.  TA_HLNG handlers receive intno as their argument; TA_ASM handlers
 * are called with no arguments -- on RISC-V the trap dispatcher already saved
 * caller-saved registers, so "ASM" here just selects the calling convention.
 */
static void knl_user_int_trampoline(void *arg)
{
	UINT intno = (UINT)(uintptr_t)arg;
	knl_user_int_t *e = &knl_user_int_tbl[intno];
	FP hdr = e->hdr;
	if (hdr == NULL) return;

	ENTER_TASK_INDEPENDENT;
	if ((e->atr & TA_HLNG) != 0) {
		((void (*)(UINT))hdr)(intno);
	} else {
		((void (*)(void))hdr)();
	}
	LEAVE_TASK_INDEPENDENT;
}

static int knl_esp_intr_level_flag(INT level)
{
	switch (level) {
	case 2:
		return ESP_INTR_FLAG_LEVEL2;
	case 3:
		return ESP_INTR_FLAG_LEVEL3;
	default:
		return ESP_INTR_FLAG_LEVEL1;
	}
}

static ER knl_alloc_user_int(UINT intno, INT level)
{
	knl_user_int_t *e = &knl_user_int_tbl[intno];
	INT alloc_level = (level >= 3) ? 3 : (level == 2) ? 2 : 1;
	int flags = knl_esp_intr_level_flag(alloc_level) | ESP_INTR_FLAG_INTRDISABLED;
	esp_err_t err;

	if (e->hdr == NULL) {
		return E_NOEXS;
	}
	if (e->handle != NULL && e->level == alloc_level) {
		return E_OK;
	}

	/* (Re)allocation runs esp_intr_free/esp_intr_alloc, which touch the
	 * IDF heap (spinlock-guarded, non-blocking, so MIE-off task context is
	 * fine) but are not interrupt-context-safe.  tk_def_int() pre-allocates
	 * at LEVEL1, so this path is reached from an ISR only when changing a
	 * source's level there -- refuse that instead of corrupting the heap. */
	if (knl_isr_nest > 0 || knl_taskindp > 0) {
		esp_rom_printf("[mtkernel] EnableInt(%u, lvl%d): level change from ISR context refused\n",
		               (unsigned)intno, (int)alloc_level);
		return E_CTX;
	}

	if (e->handle != NULL) {
		esp_intr_free(e->handle);
		e->handle = NULL;
	}

	err = esp_intr_alloc(intno, flags, knl_user_int_trampoline,
	                     (void *)(uintptr_t)intno, &e->handle);
	if (err != ESP_OK) {
		/* Surfaced as tk_def_int()'s return code on the pre-allocation
		 * path; EnableInt() is void, so from there this log is the only
		 * signal.  esp_rom_printf: no heap, no locks. */
		esp_rom_printf("[mtkernel] int source %u lvl%d: esp_intr_alloc failed (0x%x)\n",
		               (unsigned)intno, (int)alloc_level, (int)err);
		e->handle = NULL;
		e->level = 0;
		return E_SYS;
	}
	e->level = alloc_level;
	return E_OK;
}

/*
 * Set or clear the user handler for an Espressif interrupt source.
 *
 * intno is an ETS_*_SOURCE enum value (range [0, N_INTVEC)); the kernel layer
 * has already CHECK_PAR'd that bound.  Passing inthdr==NULL releases the
 * source.  Redefining an already-registered source frees the old IDF handle
 * first so esp_intr_alloc doesn't refuse on re-registration.
 *
 * The interrupt-matrix slot is allocated HERE, eagerly, at LEVEL1 (disabled),
 * so that allocation failure surfaces as tk_def_int()'s return code -- the
 * void EnableInt() cannot report it.  This also makes the common
 * EnableInt(intno, 1) call pure enable (ISR-safe); only enabling at level
 * 2..3 re-allocates, which must happen from task context.  LEVEL1..LEVEL3
 * are used so handlers remain C-callable through the IDF dispatcher.
 * Drivers needing edge-triggered or IRAM-resident handlers should call
 * esp_intr_alloc() directly.  Cost: each defined source holds one of the 31
 * routable CPU interrupt slots even before EnableInt().
 */
EXPORT ER knl_define_inthdr(INT intno, ATR intatr, FP inthdr)
{
	knl_user_int_t *e = &knl_user_int_tbl[intno];
	ER er;

	if (e->handle != NULL) {
		esp_intr_free(e->handle);
		e->handle = NULL;
	}
	e->hdr = NULL;
	e->atr = 0;
	e->level = 0;

	if (inthdr == NULL) {
		return E_OK;
	}

	e->hdr = inthdr;
	e->atr = intatr;

	er = knl_alloc_user_int(intno, 1);
	if (er != E_OK) {
		/* Leave the source unregistered so the failure is not masked. */
		e->hdr = NULL;
		e->atr = 0;
	}
	return er;
}

EXPORT void knl_return_inthdr(void)
{
	/* No epilogue work needed on RISC-V; trap exit handles it. */
}

/*
 * SYSTIMER alarm ISR thunk -- called by the IDF interrupt-matrix dispatcher
 * (registered in sys_timer.c via esp_intr_alloc).  Wraps knl_timer_handler
 * with the task-independent flag so kernel time-event handling runs in ISR
 * context per the micro T-Kernel contract.
 */
EXPORT void knl_systim_inthdr(void)
{
	UW periods;

	ENTER_TASK_INDEPENDENT;
#if CONFIG_ESP_INT_WDT
	/* Feed the INT-WDT via IDF's tick-hook chain, exactly as the FreeRTOS
	 * tick ISR would.  Once per interrupt, not per caught-up period. */
	esp_vApplicationTickHook();
#endif
	periods = knl_get_hw_timer_elapsed_periods_impl();
	if (periods == 0) {
		/* Early/spurious wake: acknowledge the alarm without advancing
		 * kernel time.  knl_timer_handler normally performs this clear;
		 * on the skip path it must happen here or the level-latched
		 * pending bit would re-fire forever. */
		knl_clear_hw_timer_interrupt_impl();
	}
	while (periods-- > 0) {
		knl_timer_handler();
	}
	LEAVE_TASK_INDEPENDENT;
}

/*
 * Generic interrupt-controller primitives required by <tk/syslib.h>.
 * Backed by IDF's per-allocation enable/disable; the per-intno IDF handle
 * lives in knl_user_int_tbl[] and is allocated on first EnableInt().
 *
 * `level` maps to ESP_INTR_FLAG_LEVEL1..LEVEL3.  Higher values are clamped to
 * LEVEL3 because IDF high-level interrupts require assembly handlers and this
 * path invokes ordinary C handlers through the interrupt dispatcher.
 *
 * ClearInt/EndOfInt are no-ops: ESP32-C3 peripherals latch and clear their
 * own interrupt status at the peripheral, not at the CPU; the trap dispatcher
 * has no separate EOI.  CheckInt reads the interrupt matrix's raw per-source
 * status registers (INTERRUPT_CORE0_INTR_STATUS_n_REG), which reflect each
 * source's request line regardless of routing/enable state.
 */
EXPORT void EnableInt(UINT intno, INT level)
{
	ER er;

	if (intno >= N_INTVEC) return;
	er = knl_alloc_user_int(intno, level);
	if (er != E_OK) return;
	intr_handle_t h = knl_user_int_tbl[intno].handle;
	if (h != NULL) (void)esp_intr_enable(h);
}

EXPORT void DisableInt(UINT intno)
{
	if (intno >= N_INTVEC) return;
	intr_handle_t h = knl_user_int_tbl[intno].handle;
	if (h != NULL) (void)esp_intr_disable(h);
}

EXPORT void ClearInt(UINT intno)
{
	(void)intno;
}

EXPORT void EndOfInt(UINT intno)
{
	(void)intno;
}

EXPORT BOOL CheckInt(UINT intno)
{
#if defined(CONFIG_IDF_TARGET_ESP32C3)
	uint32_t reg;

	if (intno >= N_INTVEC) return FALSE;
	reg = (intno < 32) ? INTERRUPT_CORE0_INTR_STATUS_0_REG
	                   : INTERRUPT_CORE0_INTR_STATUS_1_REG;
	return ((REG_READ(reg) >> (intno & 31)) & 1u) ? TRUE : FALSE;
#else
	/* Raw source-status register layout not verified for this target. */
	(void)intno;
	return FALSE;
#endif
}

#endif
