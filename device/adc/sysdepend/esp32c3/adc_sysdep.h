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
 *	adc_sysdep.h
 *	A/D converter device driver
 *	System-dependent definition for ESP32-C3
 *
 *	One unit (ADC1, /adca) with 5 channels mapped to GPIO0..GPIO4.
 *	ADC2 is omitted -- on the C3 it is shared with the WiFi radio and
 *	requires arbitration that doesn't fit the simple poll-and-read model
 *	this driver uses.
 */

#ifndef __DEV_ADC_ESP32C3_H__
#define __DEV_ADC_ESP32C3_H__

#define DEV_ADC_UNITNM	(1)	/* Number of device units */
#define DEV_ADC_1	(0)	/* /adca = ADC1 (GPIO0..GPIO4) */

#define ADC_CH_NUM	(5)	/* Channels per unit */

#endif		/* __DEV_ADC_ESP32C3_H__ */
