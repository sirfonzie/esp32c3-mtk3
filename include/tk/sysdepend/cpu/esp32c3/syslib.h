#ifndef __TK_SYSLIB_CPU_DEPEND_H__
#define __TK_SYSLIB_CPU_DEPEND_H__

#include "../core/riscv_esp/syslib.h"

#define MIN_INTNO	0
#define MAX_INTNO	(N_INTVEC - 1)

#define IM_EDGE		0x0000
#define IM_HI		0x0002
#define IM_LOW		0x0001
#define IM_BOTH		0x0003

Inline void out_w(UW port, UW data) { *(volatile UW *)port = data; }
Inline void out_h(UW port, UH data) { *(volatile UH *)port = data; }
Inline void out_b(UW port, UB data) { *(volatile UB *)port = data; }

Inline UW in_w(UW port) { return *(volatile UW *)port; }
Inline UH in_h(UW port) { return *(volatile UH *)port; }
Inline UB in_b(UW port) { return *(volatile UB *)port; }

#endif
