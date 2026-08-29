/*
 * ble_beacon.c — NimBLE non-connectable broadcaster
 *
 * This file intentionally does NOT include <tk/tkernel.h>.  The µT-Kernel
 * include tree shadows the BSD sys/queue.h that NimBLE headers require.
 * µT-Kernel ABI functions are forward-declared and resolved at link time.
 *
 * Called from usermain.c (in main/) which owns all µT-Kernel task setup.
 */

#include <string.h>
#include "nvs_flash.h"
#include "esp_rom_sys.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "services/gap/ble_svc_gap.h"

/* Forward declarations — resolved at link time by mtkernel + freertos_shim */
extern void shim_ble_patch_modules_funcs(void);
extern int  tk_slp_tsk(int tmout);  /* TMO_FEVR = -1 */

#define BEACON_NAME     "MTK3-Beacon"
#define ADV_INTERVAL_MS 100

/* Manufacturer-specific payload: test company ID 0xFFFF + "MTK3" magic */
static const uint8_t s_mfr_data[] = { 0xFF, 0xFF, 'M', 'T', 'K', '3' };

static void start_advertising(void)
{
    struct ble_hs_adv_fields fields = {};
    fields.flags                 = BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl            = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    fields.name                  = (uint8_t *)BEACON_NAME;
    fields.name_len              = sizeof(BEACON_NAME) - 1;
    fields.name_is_complete      = 1;
    fields.mfg_data              = s_mfr_data;
    fields.mfg_data_len          = sizeof(s_mfr_data);

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        esp_rom_printf("[beacon] adv set_fields failed: %d\n", rc);
        return;
    }

    struct ble_gap_adv_params params = {};
    params.conn_mode = BLE_GAP_CONN_MODE_NON;
    params.disc_mode = BLE_GAP_DISC_MODE_NON;
    params.itvl_min  = BLE_GAP_ADV_ITVL_MS(ADV_INTERVAL_MS);
    params.itvl_max  = BLE_GAP_ADV_ITVL_MS(ADV_INTERVAL_MS);

    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                           &params, NULL, NULL);
    if (rc != 0) {
        esp_rom_printf("[beacon] adv start failed: %d\n", rc);
        return;
    }
    esp_rom_printf("[beacon] broadcasting '%s' every %d ms\n",
                   BEACON_NAME, ADV_INTERVAL_MS);
    esp_rom_printf("[beacon] open nRF Connect -> Scanner to see it\n");
}

static void on_sync(void)
{
    esp_rom_printf("[beacon] NimBLE ready\n");
    start_advertising();
}

static void on_reset(int reason)
{
    esp_rom_printf("[beacon] NimBLE reset: %d\n", reason);
}

static void nimble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* Called from the µT-Kernel beacon task in main/usermain.c */
void ble_beacon_run(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    err = nimble_port_init();
    if (err != ESP_OK) {
        esp_rom_printf("[beacon] nimble_port_init failed: %d\n", (int)err);
        return;
    }

    shim_ble_patch_modules_funcs();

    ble_svc_gap_init();
    ble_svc_gap_device_name_set(BEACON_NAME);

    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    nimble_port_freertos_init(nimble_host_task);

    /* NimBLE host task is now running (via freertos_shim → µT-Kernel task).
     * Sleep this task forever — the beacon runs independently. */
    tk_slp_tsk(-1);
}
