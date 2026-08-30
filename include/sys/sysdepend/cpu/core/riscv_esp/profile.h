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

#ifndef __SYS_PROFILE_CORE_H__
#define __SYS_PROFILE_CORE_H__

#define TK_ALLOW_MISALIGN	(ALLOW_MISALIGN)
#define TK_BIGENDIAN		(BIGENDIAN)

#define TK_SUPPORT_FPU		FALSE
#define TK_SUPPORT_COP0		FALSE
#define TK_SUPPORT_COP1		FALSE
#define TK_SUPPORT_COP2		FALSE
#define TK_SUPPORT_COP3		FALSE

#define TK_SUPPORT_REGOPS	TRUE
#define TK_SUPPORT_ASM		FALSE

#define TK_SUPPORT_INTCTRL	TRUE
#define TK_HAS_ENAINTLEVEL	TRUE
#define TK_SUPPORT_CPUINTLEVEL	TRUE
#define TK_SUPPORT_CTRLINTLEVEL	FALSE
#define TK_SUPPORT_INTMODE	FALSE

#define TK_SUPPORT_CACHECTRL	FALSE
#define TK_SUPPORT_SETCACHEMODE	FALSE
#define TK_SUPPORT_WBCACHE	FALSE
#define TK_SUPPORT_WTCACHE	FALSE

#define TK_MEM_RNG0		0
#define TK_MEM_RNG1		0
#define TK_MEM_RNG2		0
#define TK_MEM_RNG3		0

#define TK_SUPPORT_MICROWAIT	TRUE

#endif
