/*
 * ble_init.c — NimBLE host + GATT server + periodic notifications
 *
 * Lives in the ble_stack component which does NOT require mtkernel.
 * µT-Kernel primitives are forward-declared using raw ABI types.
 *
 * Custom stats service:
 *   Service UUID         0xFF00  "MTK3 Stats"
 *   Characteristic UUID  0xFF01  R+N  "heap=NNNN up=Ns"
 *
 * Device Information Service (0x180A):
 *   0x2A26  Firmware Revision  "MTK3-0.1"
 *   0x2A27  Hardware Revision  "ESP32-C3"
 *   0x2A28  Software Revision  "NimBLE/uTK3"
 */

#include <string.h>
#include <stdio.h>
#include "nvs_flash.h"
#include "esp_err.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

/* Defined in freertos_shim/shim_ble_patch.c; forward-declared here to avoid
 * adding a cross-component header dependency (would pull mtkernel includes and
 * shadow the BSD sys/queue.h that NimBLE headers need). */
extern void shim_ble_patch_modules_funcs(void);

extern int  tk_dly_tsk(unsigned long dlytim);
extern void tk_ext_tsk(void);

#define UUID16(x)  BLE_UUID16_DECLARE(x)

/* -------------------------------------------------------------------------
 * State
 * ------------------------------------------------------------------------- */
static uint8_t              s_own_addr_type;
static uint16_t             s_conn_handle  = BLE_HS_CONN_HANDLE_NONE;
static uint16_t             s_stats_handle;      /* filled by ble_gatts */
static struct ble_npl_callout s_notify_callout;

static void start_advertising(void);

/* -------------------------------------------------------------------------
 * GATT access callbacks
 * ------------------------------------------------------------------------- */
static int stats_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                            struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)arg;
    char buf[48];
    uint32_t heap    = (uint32_t)esp_get_free_heap_size();
    uint32_t uptime  = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    snprintf(buf, sizeof(buf), "heap=%lu up=%lus",
             (unsigned long)heap, (unsigned long)uptime);
    int rc = os_mbuf_append(ctxt->om, buf, (uint16_t)strlen(buf));
    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int dis_read_cb(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle;
    const char *str = (const char *)arg;
    esp_rom_printf("[ble] GATT read: '%s'\n", str);
    int rc = os_mbuf_append(ctxt->om, str, (uint16_t)strlen(str));
    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

/* -------------------------------------------------------------------------
 * GATT service table
 * ------------------------------------------------------------------------- */
static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        /* Custom stats service — UUID 0xFF00 */
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = UUID16(0xFF00),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                /* Stats characteristic — UUID 0xFF01, readable + notifiable */
                .uuid       = UUID16(0xFF01),
                .access_cb  = stats_access_cb,
                .val_handle = &s_stats_handle,
                .flags      = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            },
            { 0 }
        },
    },
    {
        /* Device Information Service — UUID 0x180A */
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = UUID16(0x180A),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid      = UUID16(0x2A26),
                .access_cb = dis_read_cb,
                .arg       = "MTK3-0.1",
                .flags     = BLE_GATT_CHR_F_READ,
            },
            {
                .uuid      = UUID16(0x2A27),
                .access_cb = dis_read_cb,
                .arg       = "ESP32-C3",
                .flags     = BLE_GATT_CHR_F_READ,
            },
            {
                .uuid      = UUID16(0x2A28),
                .access_cb = dis_read_cb,
                .arg       = "NimBLE/uTK3",
                .flags     = BLE_GATT_CHR_F_READ,
            },
            { 0 }
        },
    },
    { 0 }
};

/* -------------------------------------------------------------------------
 * Periodic notification callout — runs inside the NimBLE event loop.
 * Sends heap + uptime to any connected + subscribed client every 5 s.
 * ------------------------------------------------------------------------- */
static void notify_callout_fn(struct ble_npl_event *ev)
{
    (void)ev;

    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        char buf[48];
        uint32_t heap   = (uint32_t)esp_get_free_heap_size();
        uint32_t uptime = (uint32_t)(esp_timer_get_time() / 1000000ULL);
        snprintf(buf, sizeof(buf), "heap=%lu up=%lus",
                 (unsigned long)heap, (unsigned long)uptime);

        struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, (uint16_t)strlen(buf));
        if (om) {
            int rc = ble_gattc_notify_custom(s_conn_handle, s_stats_handle, om);
            if (rc == 0) {
                esp_rom_printf("[ble] notify -> %s\n", buf);
            } else {
                os_mbuf_free_chain(om);
                esp_rom_printf("[ble] notify rc=%d (client not subscribed?)\n", rc);
            }
        }
    }

    /* Re-arm for next notification in 5 s. */
    ble_npl_callout_reset(&s_notify_callout, ble_npl_time_ms_to_ticks32(5000));
}

/* -------------------------------------------------------------------------
 * GAP event handler
 * ------------------------------------------------------------------------- */
static int gap_event_handler(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            esp_rom_printf("[ble] GATT connected handle=%d\n",
                           s_conn_handle);
        } else {
            start_advertising();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        esp_rom_printf("[ble] GAP: disconnect reason=%d\n",
                       event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        start_advertising();
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        start_advertising();
        break;

    default:
        break;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * start_advertising
 * ------------------------------------------------------------------------- */
static void start_advertising(void)
{
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields  fields;
    int rc;

    memset(&fields, 0, sizeof(fields));
    fields.flags                 = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl            = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    const char *name  = ble_svc_gap_device_name();
    fields.name       = (uint8_t *)name;
    fields.name_len   = (uint8_t)strlen(name);
    fields.name_is_complete = 1;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        esp_rom_printf("[ble] adv set_fields FAILED: %d\n", rc);
        return;
    }

    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, gap_event_handler, NULL);
    if (rc != 0) {
        esp_rom_printf("[ble] adv start FAILED: %d\n", rc);
        return;
    }
    esp_rom_printf("[ble] advertising as 'MTK3-C3'\n");
}

/* -------------------------------------------------------------------------
 * NimBLE host callbacks
 * ------------------------------------------------------------------------- */
static void on_reset(int reason)
{
    esp_rom_printf("[ble] NimBLE reset: reason=%d\n", reason);
}

static void on_sync(void)
{
    int rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        esp_rom_printf("[ble] infer addr type FAILED: %d\n", rc);
        return;
    }
    esp_rom_printf("[ble] NimBLE ready\n");

    /* Arm the notification callout — first fire in 5 s. */
    ble_npl_callout_reset(&s_notify_callout, ble_npl_time_ms_to_ticks32(5000));

    start_advertising();
}

/* -------------------------------------------------------------------------
 * NimBLE host task
 * ------------------------------------------------------------------------- */
static void nimble_host_task(void *param)
{
    (void)param;
    esp_rom_printf("[ble] nimble_host task started\n");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* -------------------------------------------------------------------------
 * ble_init_run() — called from ble_task.c
 * ------------------------------------------------------------------------- */
void ble_init_run(void)
{
    tk_dly_tsk(1000);

    esp_rom_printf("[ble] initialising NimBLE + GATT\n");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        esp_rom_printf("[ble] nvs_flash_init FAILED: %d\n", (int)err);
        tk_ext_tsk(); return;
    }

    err = nimble_port_init();
    if (err != ESP_OK) {
        esp_rom_printf("[ble] nimble_port_init FAILED: %d\n", (int)err);
        tk_ext_tsk(); return;
    }
    esp_rom_printf("[ble] nimble_port_init OK\n");

    /* Patch ke_event_schedule slot in the DRAM modules_funcs dispatch table.
     * btdm_controller_init (called by nimble_port_init) copies r_modules_funcs_ro
     * to DRAM and sets r_modules_funcs_p.  Our safe version clears unhandled
     * events (e.g. event 6/CCA with no registered callback) instead of crashing
     * through r_assert_err → plf_funcs[158] = NULL → MEPC=0x00000000. */
    shim_ble_patch_modules_funcs();

    /* Standard GAP + GATT services. */
    ble_svc_gap_init();
    ble_svc_gatt_init();

    /* Register custom services. */
    int rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc != 0) { esp_rom_printf("[ble] count_cfg FAILED: %d\n", rc); tk_ext_tsk(); return; }
    rc = ble_gatts_add_svcs(s_gatt_svcs);
    if (rc != 0) { esp_rom_printf("[ble] add_svcs FAILED: %d\n", rc); tk_ext_tsk(); return; }
    esp_rom_printf("[ble] GATT services registered (DIS + stats)\n");

    /*
     * Initialise the notification callout against the default NimBLE event
     * queue.  The callout fires inside the nimble_host task's event loop, so
     * all NimBLE API calls within notify_callout_fn are safe.
     */
    ble_npl_callout_init(&s_notify_callout,
                         nimble_port_get_dflt_eventq(),
                         notify_callout_fn,
                         NULL);
    esp_rom_printf("[ble] notify callout initialised\n");

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb  = on_sync;

    ble_svc_gap_device_name_set("MTK3-C3");

    nimble_port_freertos_init(nimble_host_task);
    esp_rom_printf("[ble] nimble_host task launched\n");

    tk_ext_tsk();
}
