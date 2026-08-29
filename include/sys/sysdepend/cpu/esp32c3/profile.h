#ifndef __SYS_DEPEND_PROFILE_CPU_H__
#define __SYS_DEPEND_PROFILE_CPU_H__

#include "../core/riscv_esp/profile.h"

#define TK_SUPPORT_IOPORT	TRUE

#if USE_PTMR
#define TK_SUPPORT_PTIMER	TRUE
#define TK_MAX_PTIMER		2	/* TIMG0/T0 = ptmr1, TIMG1/T0 = ptmr2 */
#else
#define TK_SUPPORT_PTIMER	FALSE
#define TK_MAX_PTIMER		0
#endif

#endif
