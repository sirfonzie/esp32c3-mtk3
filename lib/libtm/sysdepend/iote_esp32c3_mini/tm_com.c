#include <tk/tkernel.h>

#if USE_TMONITOR
#include "../../libtm.h"

#ifdef IOTE_ESP32C3_MINI

#include "esp_rom_sys.h"
#include "esp_rom_serial_output.h"

EXPORT void tm_snd_dat(const UB *buf, INT size)
{
	for (INT i = 0; i < size; i++) {
		esp_rom_output_tx_one_char(buf[i]);
	}
}

EXPORT void tm_rcv_dat(UB *buf, INT size)
{
	for (INT i = 0; i < size; i++) {
		while (esp_rom_output_rx_one_char(&buf[i]) != 0) { }
	}
}

EXPORT void tm_com_init(void)
{
	/* Console port is configured by IDF bootloader.
	 * With CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y (sdkconfig.defaults),
	 * esp_rom_output_tx/rx_one_char route through USB-Serial-JTAG (USB-C).
	 * No external UART adapter required. */
}

#endif
#endif
