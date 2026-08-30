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

/*
 *	ser_mode_sysdep.h
 *	Serial communication device driver
 *	Communication mode definition for ESP32-C3
 *
 *	The framework's DEVCNF_SER_MODE in ser_cnf.h ORs several mode flags
 *	together; we pick encodings so the common case (8N1, no flow control)
 *	is just zero, which the driver then translates into UART CONF0 bits.
 */

#ifndef __DEV_SER_MODE_ESP32C3_H__
#define __DEV_SER_MODE_ESP32C3_H__

/* Word length -- field at bits [1:0] */
#define	DEV_SER_MODE_8BIT	(0x00)
#define	DEV_SER_MODE_7BIT	(0x01)
#define	DEV_SER_MODE_6BIT	(0x02)
#define	DEV_SER_MODE_5BIT	(0x03)
#define	DEV_SER_MODE_WL_MASK	(0x03)

/* Stop bits -- bit [2] */
#define	DEV_SER_MODE_1STOP	(0x00)
#define	DEV_SER_MODE_2STOP	(0x04)
#define	DEV_SER_MODE_STOP_MASK	(0x04)

/* Parity -- bits [4:3] */
#define	DEV_SER_MODE_PNON	(0x00)
#define DEV_SER_MODE_PODD	(0x08)
#define DEV_SER_MODE_PEVEN	(0x10)
#define	DEV_SER_MODE_P_MASK	(0x18)

/* Hardware flow control -- bits [6:5].  Not wired in the loopback bring-up
 * (the C3 UART supports HW flow control but the pin matrix is not configured
 * here); kept as named constants so DEVCNF_SER_MODE compiles unchanged. */
#define	DEV_SER_MODE_CTSEN	(0x20)
#define	DEV_SER_MODE_RTSEN	(0x40)

/* Communication errors reported via dev_ser_notify_err */
#define	DEV_SER_ERR_PE		(1<<0)	/* Parity error */
#define	DEV_SER_ERR_FE		(1<<1)	/* Framing error */
#define	DEV_SER_ERR_NF		(1<<2)	/* Start-bit noise detection */
#define	DEV_SER_ERR_OE		(1<<3)	/* Overrun (RX FIFO) */

#endif		/* __DEV_SER_MODE_ESP32C3_H__ */
