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

#ifndef __SYS_SYSDEPEND_MACHINE_H__
#define __SYS_SYSDEPEND_MACHINE_H__

#define IOTE_M5STAMP_C3		1
#define CPU_ESP32C3		1
#define CPU_CORE_RISCV_ESP	1

#define TARGET_DIR		iote_m5stamp_c3
#define KNL_SYSDEP_PATH		iote_m5stamp_c3

#include "../cpu/esp32c3/machine.h"

#endif
