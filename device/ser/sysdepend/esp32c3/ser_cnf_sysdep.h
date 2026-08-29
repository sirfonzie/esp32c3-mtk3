/*
 *	ser_cnf_sysdep.h
 *	Serial Device configuration file for ESP32-C3
 */
#ifndef __DEV_SER_CNF_ESP32C3_H__
#define __DEV_SER_CNF_ESP32C3_H__

#define	DEVCNF_UART1_INTPRI	1	/* EnableInt maps this to ESP interrupt LEVEL1..LEVEL3 */

/* T-Monitor uses the ROM UART path (tm_com.c -> esp_rom_output_tx_one_char),
 * not a device-manager channel.  -1 tells ser.c that no /serX is reserved
 * for the monitor, so DEV_SER_UNIT0 (UART1) is fully available to user code. */
#define	DEVCNF_SER_DBGUN	-1

/* GPIO matrix routing for UART1 TX/RX.  GPIO20/21 are the C3's IO_MUX
 * defaults for UART0 -- we're matrix-routing UART1 there instead.  No
 * conflict because the ROM bootloader's UART0 transmit happens long
 * before setup_pins() runs.  A TX<->RX jumper between GPIO21 and
 * GPIO20 enables external loopback. */
#define	DEVCNF_SER_TX_PIN	21
#define	DEVCNF_SER_RX_PIN	20

#endif		/* __DEV_SER_CNF_ESP32C3_H__ */
