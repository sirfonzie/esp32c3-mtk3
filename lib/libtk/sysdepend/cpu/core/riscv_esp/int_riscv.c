#include <sys/machine.h>
#ifdef CPU_CORE_RISCV_ESP

#include <tk/tkernel.h>

#include "soc/soc.h"
#include "soc/interrupt_reg.h"

/*
 * disint() / enaint() control the RISC-V machine-mode interrupt enable
 * (mstatus.MIE).  disint clears it and returns the prior bit so callers can
 * restore symmetrically.  This is the lowest-level CPU interrupt gate.
 *
 * SetCpuIntLevel() / GetCpuIntLevel() control the ESP32-C3 interrupt
 * threshold register used by ENABLE_INTERRUPT_UPTO() and by the kernel's
 * lock/dispatch-state checks.  Normal task context uses INTLEVEL_EI; full
 * CPU-lock state uses INTLEVEL_DI.
 */

EXPORT UW disint(void)
{
	UW prev;
	Asm("csrrci %0, mstatus, %1" : "=r"(prev) : "i"(MSTATUS_MIE) : "memory");
	return prev & MSTATUS_MIE;
}

EXPORT void enaint(UW prev)
{
	if (prev & MSTATUS_MIE) {
		Asm("csrrsi zero, mstatus, %0" :: "i"(MSTATUS_MIE) : "memory");
	}
}

EXPORT void SetCpuIntLevel(INT level)
{
	if (level < INTLEVEL_EI) {
		level = INTLEVEL_EI;
	} else if (level > INTLEVEL_DI) {
		level = INTLEVEL_DI;
	}
	UW prev = disint();
	REG_WRITE(INTERRUPT_CURRENT_CORE_INT_THRESH_REG, (uint32_t)level);
	(void)REG_READ(INTERRUPT_CURRENT_CORE_INT_THRESH_REG);
	enaint(prev);
}

EXPORT INT GetCpuIntLevel(void)
{
	return (INT)REG_READ(INTERRUPT_CURRENT_CORE_INT_THRESH_REG);
}

#endif
