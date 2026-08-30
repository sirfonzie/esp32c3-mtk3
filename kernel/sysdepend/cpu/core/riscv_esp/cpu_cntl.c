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

#include <stddef.h>
#include <sys/inittask.h>

#include "kernel.h"
#include "../../../sysdepend.h"

_Static_assert(INITTASK_ITSKPRI == 10,
    "Shadow sys/inittask.h not applied — check INCLUDE_DIRS order in CMakeLists.txt");
_Static_assert(INITTASK_STKSZ == 8*1024,
    "Shadow sys/inittask.h not applied — check INCLUDE_DIRS order in CMakeLists.txt");

#include "hal/crosscore_int_ll.h"
#include "soc/interrupt_reg.h"
#include "esp_idf_version.h"

/*
 * This file (and the freertos_shim's rtos_int_enter) relies on the INTERNAL
 * contract of IDF's components/riscv/vectors.S, which is not a published
 * API: the rtos_int_enter/rtos_int_exit call sites and register ABI, the
 * full-GPR save with gp excluded from restore, and the threshold-restore-
 * before-rtos_int_exit ordering.  All of it was verified by inspection and
 * on hardware against ESP-IDF v6.1-dev.  On a major-version bump, re-verify
 * vectors.S against the "IDF ABI dependencies" checklist in PORT_ESP32C3.md
 * before defining MTK3_SKIP_IDF_VERSION_CHECK to build.
 */
#if !defined(MTK3_SKIP_IDF_VERSION_CHECK)
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(6, 0, 0) || \
    ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(7, 0, 0)
#error "Unverified ESP-IDF major version: re-check the vectors.S ABI (PORT_ESP32C3.md, 'IDF ABI dependencies'), then define MTK3_SKIP_IDF_VERSION_CHECK."
#endif
#endif

EXPORT W knl_taskindp = 0;
EXPORT W knl_isr_nest = 0;

EXPORT void *knl_lowmem_top    = NULL;
EXPORT void *knl_lowmem_limit  = NULL;

/*
 * Dedicated interrupt stack.  rtos_int_enter (freertos_shim/shim_stubs.c)
 * switches sp here on the first nesting level, exactly as IDF's FreeRTOS
 * port does, so ISRs -- including nested WiFi/BLE LEVEL3 handlers -- no
 * longer consume the interrupted task's stack.  vectors.S is built for
 * this: it saves the trap frame on the interrupted stack *before* calling
 * rtos_int_enter, and expects rtos_int_exit to put the frame pointer (or
 * the next task's) back into sp.  Nested interrupts stay on this stack.
 * Must live in DRAM (it is used while the flash cache is disabled) and be
 * 16-byte aligned per the RISC-V ABI.
 */
#define KNL_INT_STACK_SIZE	4096
static UW knl_int_stack[KNL_INT_STACK_SIZE / sizeof(UW)] __attribute__((aligned(16)));
EXPORT void *knl_int_stack_top = &knl_int_stack[KNL_INT_STACK_SIZE / sizeof(UW)];
EXPORT void *knl_int_saved_sp  = NULL;

/*
 * knl_dispatch -- pend the cross-core software IRQ (FROM_CPU_INTR0).
 * The dispatcher trigger.  When the IRQ fires, IDF's trap entry pushes the
 * current task's RvExcFrame on the running stack, the ISR (a no-op
 * registered via esp_intr_alloc in interrupt.c) clears the cause, then
 * __wrap_rtos_int_exit below sees knl_ctxtsk != knl_schedtsk and swaps sp
 * to the new task's saved frame, which the IDF trap exit pops and mrets to.
 *
 * Edge case: if the blocking task is the ONLY ready task (all others are
 * sleeping or waiting), knl_make_non_ready() sets knl_schedtsk = NULL.
 * __wrap_rtos_int_exit would then hit "beqz t1,3f" and keep the current
 * task running despite it being in TS_WAIT, causing tk_wai_sem/tk_rcv_mbf
 * to return spurious E_TMOUT with TMO_FEVR.
 * Fix: when knl_schedtsk == NULL, briefly re-enable interrupts and WFI so
 * that a timer/DMA ISR can make another task ready.  The ISR tail handler
 * (__wrap_rtos_int_exit) will do the actual context switch; we resume here
 * only once the kernel has woken us from the wait queue (knl_schedtsk != NULL).
 */
EXPORT void knl_dispatch(void)
{
	/*
	 * Task-independent (ISR) context: the pending context switch is performed
	 * by __wrap_rtos_int_exit when the current interrupt unwinds, exactly as
	 * armv7m defers to PendSV.  Dispatching here is wrong and -- because the
	 * schedtsk==NULL branch below WFIs -- would park the CPU with the timer
	 * interrupt still in service, wedging the whole system.  Defer.
	 */
	if (knl_taskindp > 0) {
		return;
	}

	while (knl_schedtsk == NULL) {
		SetCpuIntLevel(INTLEVEL_EI);
		Asm("csrsi mstatus, %0" :: "i"(MSTATUS_MIE) : "memory");
		Asm("wfi"               :::                    "memory");
		Asm("csrci mstatus, %0" :: "i"(MSTATUS_MIE) : "memory");
	}
	crosscore_int_ll_trigger_interrupt(0);
}

/*
 * knl_force_dispatch -- first dispatch.  Called from knl_main once the kernel
 * has created and queued the initial task.  We have no current task, so the
 * wrap will skip the save side and just load sp from knl_schedtsk.
 *
 * Interrupts are disabled on entry (sysinit did so very early).  We enable
 * them, trigger the SW IRQ; it fires immediately, the wrap switches us into
 * the initial task, we never return here.  The wfi loop is just paranoia.
 */
EXPORT void knl_force_dispatch(void)
{
	knl_ctxtsk = NULL;
	knl_dispatch_disabled = 0;
	knl_taskindp = 0;
	knl_isr_nest = 0;

	/* Restore the normal ESP interrupt threshold and enable MIE before
	 * triggering so the SW IRQ fires with MIE=1.
	 * Hardware captures MPIE=1 on entry; mret then sets MIE←MPIE=1,
	 * ensuring the init task runs with interrupts enabled. */
	SetCpuIntLevel(INTLEVEL_EI);
	Asm("csrsi mstatus, %0" :: "i"(MSTATUS_MIE) : "memory");
	crosscore_int_ll_trigger_interrupt(0);

	for (;;) {
		Asm("wfi" ::: "memory");
	}
}

/*
 * Replacement for FreeRTOS's rtos_int_exit (installed via -Wl,--wrap).
 * Performs the micro T-Kernel task switch on every interrupt's tail edge.
 *
 *   ABI (from components/riscv/vectors.S):
 *     a0 = saved mstatus  (we return this verbatim)
 *     a1 = abstract context from rtos_int_enter  (we ignore)
 *     sp  = interrupt stack (rtos_int_enter switched to it on the first
 *           nesting level; the interrupted task's saved RvExcFrame pointer
 *           is in knl_int_saved_sp).  s1/s2 must be preserved.
 *
 *   On exit:
 *     sp  = pointer to the frame IDF's trap exit should pop and mret to:
 *           knl_int_saved_sp when no switch happens (or on a nested exit,
 *           where sp already points at the nested frame on the interrupt
 *           stack), or the *next* task's saved frame on a switch
 *     knl_ctxtsk is NEVER set to NULL: if knl_schedtsk == NULL (ready queue
 *     empty), no switch occurs and the current task keeps running.
 *
 * CPU interrupt threshold across a switch: vectors.S restored the
 * interrupted task's threshold just before calling us, so on the save side
 * the live INTC threshold register IS the outgoing task's SetCpuIntLevel
 * state -- capture it into tskctxb.intlevel.  On the load side write the
 * incoming task's saved intlevel back, so levels 1..3 survive preemption
 * (new tasks start at INTLEVEL_EI, see knl_setup_context).  On the
 * no-switch paths the threshold is left exactly as vectors.S restored it.
 */
__attribute__((naked, used, section(".iram1.text")))
void __wrap_rtos_int_exit(unsigned long mstatus_in, unsigned long ctx_in)
{
	(void)mstatus_in; (void)ctx_in;
	Asm(
		/* Leave task-independent (ISR) context: undo the rtos_int_enter
		 * increments.  Use knl_isr_nest as the hardware ISR nesting gate, and
		 * also require knl_taskindp to be back at zero so any T-Kernel
		 * task-independent section still defers the tail switch. */
		"la    t0, knl_taskindp         \n"
		"lw    t1, 0(t0)                \n"
		"addi  t1, t1, -1               \n"
		"sw    t1, 0(t0)                \n"
		"la    t3, knl_isr_nest         \n"
		"lw    t4, 0(t3)                \n"
		"addi  t4, t4, -1               \n"
		"sw    t4, 0(t3)                \n"
		"bnez  t4, 3f                   \n"   /* nested exit: frame is on the int stack, sp already right */
		"la    t0, knl_int_saved_sp     \n"
		"lw    sp, 0(t0)                \n"   /* leave the interrupt stack: sp = interrupted frame */
		"bnez  t1, 3f                   \n"   /* still task-independent */
		"lw    t2, knl_dispatch_disabled\n"
		"bnez  t2, 3f                   \n"   /* if dispatch disabled, do not switch */
		"lw    t0, knl_ctxtsk           \n"
		"lw    t1, knl_schedtsk         \n"
		"beq   t0, t1, 3f               \n"   /* no switch needed */
		"beqz  t1, 3f                   \n"   /* no next task -> keep current running */
		"beqz  t0, 1f                   \n"   /* no current task -> skip save */
		"sw    sp, %[ssp_off](t0)       \n"
		"li    t2, %[thresh_reg]        \n"
		"lw    t3, 0(t2)                \n"
		"sw    t3, %[il_off](t0)        \n"   /* save outgoing task's threshold */
	"1:                                 \n"
		"la    t2, knl_ctxtsk           \n"
		"sw    t1, 0(t2)                \n"   /* knl_ctxtsk = knl_schedtsk */
		"lw    sp, %[ssp_off](t1)       \n"
		"li    t0, %[thresh_reg]        \n"
		"lw    t2, %[il_off](t1)        \n"
		"sw    t2, 0(t0)                \n"   /* restore incoming task's threshold */
		"fence                          \n"
	"3:                                 \n"
		"ret                            \n"
		:
		: [ssp_off] "i"(offsetof(TCB, tskctxb)),
		  [il_off]  "i"(offsetof(TCB, tskctxb.intlevel)),
		  [thresh_reg] "i"(INTERRUPT_CURRENT_CORE_INT_THRESH_REG)
	);
}

/*
 * Panic diagnostic (installed via -Wl,--wrap=esp_panic_handler in
 * components/mtkernel/CMakeLists.txt).  There is no µT-Kernel task exception
 * containment on this port -- any fault lands in IDF's panic handler and
 * reboots -- so at least identify WHICH task was running, its stack base,
 * and the kernel's context flags before chaining to the real handler.
 * panic_info_t is deliberately opaque (esp_private header); we only pass it
 * through.  esp_rom_printf: no heap, no locks, safe in panic context.
 */
#include "esp_rom_sys.h"

IMPORT void __real_esp_panic_handler(void *info);

EXPORT void __wrap_esp_panic_handler(void *info)
{
	TCB *t = knl_ctxtsk;

	if (t != NULL) {
		esp_rom_printf("[mtkernel] panic: ctxtsk id=%d state=%d isstack=%p"
		               " isr_nest=%d taskindp=%d dispatch_disabled=%d\n",
		               (int)t->tskid, (int)t->state, t->isstack,
		               (int)knl_isr_nest, (int)knl_taskindp,
		               (int)knl_dispatch_disabled);
	} else {
		esp_rom_printf("[mtkernel] panic: before first dispatch"
		               " (isr_nest=%d taskindp=%d)\n",
		               (int)knl_isr_nest, (int)knl_taskindp);
	}
	__real_esp_panic_handler(info);
}

#endif
