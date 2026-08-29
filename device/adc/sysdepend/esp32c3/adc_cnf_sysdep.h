/*
 *	adc_cnf_sysdep.h
 *	A/D converter configuration for ESP32-C3
 *
 *	Defaults match the IDF "least surprising" oneshot setup: 12-bit width
 *	(the only width the C3 SAR supports) and the widest attenuation so
 *	0..~3.1 V at the pin maps to 0..4095 with the standard reference.
 */

#ifndef __DEV_ADC_CNF_ESP32C3_H__
#define __DEV_ADC_CNF_ESP32C3_H__

#define DEVCNF_ADC_BITWIDTH	(12)	/* C3 only supports 12-bit oneshot */
#define DEVCNF_ADC_ATTEN	(3)	/* ADC_ATTEN_DB_12: 0..~3.1V range */

#endif		/* __DEV_ADC_CNF_ESP32C3_H__ */
