/*
 * ble_gatt.c — NimBLE connectable GATT server
 *
 * This file intentionally does NOT include <tk/tkernel.h>.
 * See ble_beacon/ble_nimble/ble_beacon.c for the reason.
 *
 * Services exposed:
 *   Device Information (0x180A)
 *     0x2A26  Firmware Revision  R    "MTK3-1.0"
 *     0x2A27  Hardware Revision  R    "ESP32-C3"
 *     0x2A28  Software Revision  R    "NimBLE/uTK3"
 *
 *   MTK3 Stats (0xFF00)
 *     0xFF01  Status  R+N  "heap=NNNN up=Ns"   (read or 3 s notify)
 *     0xFF02  Echo    R+W  write any string, read it back
 *
 * nRF Connect walk-through:
 *   1. Scanner → find "MTK3-GATT" → Connect
 *   2. Client tab → expand "Unknown Service" (UUID 0xFF00)
 *   3. Read 0xFF01 → shows free heap + uptime
 *   4. Subscribe (bell icon) on 0xFF01 → receives updates every 3 s
 *   5. Write UTF-8 string to 0xFF02 → Read 0xFF02 → echoed back
 *   6. Expand "Device Information" (0x180A) → read firmware/hardware strings
 */

#include <string.h>
#include <stdio.h>
#include "nvs_flash.h"
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

extern void shim_ble_patch_modules_funcs(void);
extern int  tk_slp_tsk(int tmout);

#define DEVICE_NAME "MTK3-GATT"
#define UUID16(x)   BLE_UUID16_DECLARE(x)

static uint8_t  s_own_addr_type;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_status_handle;
static char     s_echo_buf[64] = "hello from MTK3";

static struct ble_npl_callout s_notify_callout;

static void start_advertising(void);

/* -------------------------------------------------------------------------
 * GATT access callbacks
 * ------------------------------------------------------------------------- */
static int status_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)arg;
    char buf[64];
    snprintf(buf, sizeof(buf), "heap=%lu up=%lus",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)(esp_timer_get_time() / 1000000ULL));
    int rc = os_mbuf_append(ctxt->om, buf, (uint16_t)strlen(buf));
    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int echo_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)arg;

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len >= sizeof(s_echo_buf)) len = (uint16_t)(sizeof(s_echo_buf) - 1);
        os_mbuf_copydata(ctxt->om, 0, len, s_echo_buf);
        s_echo_buf[len] = '\0';
        esp_rom_printf("[gatt] echo write: '%s'\n", s_echo_buf);
        return 0;
    }
    int rc = os_mbuf_append(ctxt->om, s_echo_buf, (uint16_t)strlen(s_echo_buf));
    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int dis_read_cb(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle;
    const char *str = (const char *)arg;
    int rc = os_mbuf_append(ctxt->om, str, (uint16_t)strlen(str));
    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

/* -------------------------------------------------------------------------
 * GATT service table
 * ------------------------------------------------------------------------- */
static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = UUID16(0xFF00),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid       = UUID16(0xFF01),
                .access_cb  = status_access_cb,
                .val_handle = &s_status_handle,
                .flags      = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            },
            {
                .uuid      = UUID16(0xFF02),
                .access_cb = echo_access_cb,
                .flags     = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
            },
            { 0 }
        },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = UUID16(0x180A),
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = UUID16(0x2A26), .access_cb = dis_read_cb,
              .arg  = "MTK3-1.0",    .flags = BLE_GATT_CHR_F_READ },
            { .uuid = UUID16(0x2A27), .access_cb = dis_read_cb,
              .arg  = "ESP32-C3",    .flags = BLE_GATT_CHR_F_READ },
            { .uuid = UUID16(0x2A28), .access_cb = dis_read_cb,
              .arg  = "NimBLE/uTK3", .flags = BLE_GATT_CHR_F_READ },
            { 0 }
        },
    },
    { 0 }
};

/* -------------------------------------------------------------------------
 * Periodic notify callout — fires inside NimBLE event loop every 3 s
 * ------------------------------------------------------------------------- */
static void notify_fn(struct ble_npl_event *ev)
{
    (void)ev;
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        char buf[64];
        snprintf(buf, sizeof(buf), "heap=%lu up=%lus",
                 (unsigned long)esp_get_free_heap_size(),
                 (unsigned long)(esp_timer_get_time() / 1000000ULL));
        struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, (uint16_t)strlen(buf));
        if (om) {
            int rc = ble_gattc_notify_custom(s_conn_handle, s_status_handle, om);
            if (rc == 0)
                esp_rom_printf("[gatt] notify: %s\n", buf);
            else
                os_mbuf_free_chain(om);
        }
    }
    ble_npl_callout_reset(&s_notify_callout, ble_npl_time_ms_to_ticks32(3000));
}

/* -------------------------------------------------------------------------
 * GAP event handler
 * ------------------------------------------------------------------------- */
static int gap_event_cb(struct ble_gap_event *ev, void *arg)
{
    (void)arg;
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) {
            s_conn_handle = ev->connect.conn_handle;
            esp_rom_printf("[gatt] connected handle=%d\n", s_conn_handle);
        } else {
            start_advertising();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        esp_rom_printf("[gatt] disconnected reason=%d\n", ev->disconnect.reason);
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

static void start_advertising(void)
{
    struct ble_hs_adv_fields fields = {};
    fields.flags                 = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl            = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    const char *name  = ble_svc_gap_device_name();
    fields.name       = (uint8_t *)name;
    fields.name_len   = (uint8_t)strlen(name);
    fields.name_is_complete = 1;
    ble_gap_adv_set_fields(&fields);

    struct ble_gap_adv_params params = {};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    int rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                                &params, gap_event_cb, NULL);
    if (rc == 0)
        esp_rom_printf("[gatt] advertising as '%s'\n", DEVICE_NAME);
    else
        esp_rom_printf("[gatt] adv start failed: %d\n", rc);
}

static void on_sync(void)
{
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    esp_rom_printf("[gatt] NimBLE ready\n");
    ble_npl_callout_reset(&s_notify_callout, ble_npl_time_ms_to_ticks32(3000));
    start_advertising();
}

static void on_reset(int reason)
{
    esp_rom_printf("[gatt] NimBLE reset: %d\n", reason);
}

static void nimble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* Called from the µT-Kernel gatt task in main/usermain.c */
void ble_gatt_run(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    err = nimble_port_init();
    if (err != ESP_OK) {
        esp_rom_printf("[gatt] nimble_port_init failed: %d\n", (int)err);
        return;
    }

    shim_ble_patch_modules_funcs();

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(DEVICE_NAME);

    ble_gatts_count_cfg(s_svcs);
    ble_gatts_add_svcs(s_svcs);

    ble_npl_callout_init(&s_notify_callout,
                         nimble_port_get_dflt_eventq(),
                         notify_fn, NULL);

    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    nimble_port_freertos_init(nimble_host_task);
    esp_rom_printf("[gatt] nimble host task launched\n");

    tk_slp_tsk(-1);
}
