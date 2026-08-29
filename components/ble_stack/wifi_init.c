/*
 * wifi_init.c — WiFi AP init, must run BEFORE nimble_port_init().
 *
 * esp_wifi_init() initialises the RF coexistence context that is also used
 * by the BLE controller.  With CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y the ROM
 * coex functions (coex_schm_status_bit_clear etc.) dereference a pointer
 * that is only set by esp_wifi_init().  Calling wifi_init_run() first means
 * the context is valid when nimble_port_init() arms the BLE controller.
 */

#include <string.h>
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_err.h"
#include "esp_rom_sys.h"

void wifi_init_run(void)
{
    esp_rom_printf("[wifi] initialising...\n");

    /* NVS is shared with BLE; handle first-boot erase gracefully. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        esp_rom_printf("[wifi] nvs_flash_init FAILED: %d\n", (int)err);
        return;
    }

    /* lwIP + event loop — one-time init shared by the whole application. */
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_ap();

    /*
     * esp_wifi_init() is what actually sets up the coex context in ROM.
     * This must complete before nimble_port_init() / esp_bt_controller_init().
     */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        esp_rom_printf("[wifi] esp_wifi_init FAILED: %d\n", (int)err);
        return;
    }

    /* Start an open AP so the phone can see "MTK3-AP" in its WiFi scan. */
    wifi_config_t wifi_cfg = {
        .ap = {
            .ssid           = "MTK3-AP",
            .ssid_len       = 7,
            .channel        = 6,
            .authmode       = WIFI_AUTH_OPEN,
            .max_connection = 4,
        },
    };
    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &wifi_cfg);
    err = esp_wifi_start();
    if (err != ESP_OK) {
        esp_rom_printf("[wifi] esp_wifi_start FAILED: %d\n", (int)err);
        return;
    }

    esp_rom_printf("[wifi] AP 'MTK3-AP' started\n");
}
