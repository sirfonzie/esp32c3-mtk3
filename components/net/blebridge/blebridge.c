/*
 * blebridge.c — NimBLE scan-all + advertise, forwarding sightings up a mesh.
 *
 * Coexistence note: esp_wifi_init() (called by espmesh_start, BEFORE this)
 * arms the RF coex context the BLE controller relies on when
 * CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y.  Order matters: mesh/WiFi first, BLE
 * second.  This file deliberately does NOT include <tk/tkernel.h> — the
 * kernel include tree ships its own sys/queue.h that shadows the BSD one
 * NimBLE needs.  uT-Kernel is never referenced here; the mesh uplink is used
 * only through the kernel-agnostic espmesh API.
 */
#include <string.h>
#include <stdio.h>
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "espmesh.h"

/* Defined in freertos_shim/shim_ble_patch.c; forward-declared (don't pull the
 * shim header — it would drag the kernel sys/queue.h in).  Clears unhandled
 * controller events instead of crashing through a NULL plf_funcs slot. */
extern void shim_ble_patch_modules_funcs(void);

static uint8_t      s_own_addr_type;
static char         s_adv_name[31] = "MTK3-C3-BLE";
static volatile bool s_synced;

/* Live status exfil — caller writes a string (no NimBLE calls); a callout in
 * the NimBLE host task copies it into the advert name so a phone can read the
 * C3 state even while the caller is blocked. */
static struct ble_npl_callout s_dbg_callout;
static char s_status[28] = "C3 boot";
static volatile unsigned s_seen;
static volatile unsigned s_fwd;
static int64_t      s_last_fwd_us;          /* rate-limit gate */

#define FWD_MIN_INTERVAL_US  (400 * 1000)   /* ≤ ~2.5 forwards/s up the mesh */

static void start_advertising(void);
static void dbg_callout_fn(struct ble_npl_event *ev);

/* -------------------------------------------------------------------------
 * Scan callback — every advertisement in range lands here.
 * ------------------------------------------------------------------------- */
static int scan_event_cb(struct ble_gap_event *ev, void *arg)
{
    (void)arg;
    if (ev->type != BLE_GAP_EVENT_DISC)
        return 0;

    s_seen++;

    /* Rate-limit what we push up the mesh so a busy RF environment can't
     * flood the uplink; we still count every sighting above. */
    int64_t now = esp_timer_get_time();
    if (now - s_last_fwd_us < FWD_MIN_INTERVAL_US)
        return 0;

    /* Only forward once we actually have a root to send to. */
    uint8_t root[6];
    if (!espmesh_get_root_addr(root))
        return 0;

    /* Pull the advertised name if present (purely for a readable report). */
    char name[24] = {0};
    struct ble_hs_adv_fields f;
    if (ble_hs_adv_parse_fields(&f, ev->disc.data, ev->disc.length_data) == 0 &&
        f.name != NULL && f.name_len > 0) {
        int n = f.name_len < (int)sizeof(name) - 1 ? f.name_len
                                                   : (int)sizeof(name) - 1;
        memcpy(name, f.name, n);
        name[n] = '\0';
    }

    const uint8_t *a = ev->disc.addr.val;   /* little-endian; print reversed */
    char msg[64];
    int len = snprintf(msg, sizeof(msg),
                       "BLE %02x:%02x:%02x:%02x:%02x:%02x rssi=%d '%s'",
                       a[5], a[4], a[3], a[2], a[1], a[0],
                       ev->disc.rssi, name);
    if (len <= 0)
        return 0;

    if (espmesh_send(root, msg, len) == 0) {
        s_fwd++;
        s_last_fwd_us = now;
        esp_rom_printf("[blebridge] fwd up: %s\n", msg);
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Continuous non-connectable advertise so a phone always sees this node.
 * ------------------------------------------------------------------------- */
static void start_advertising(void)
{
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags                 = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl            = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    fields.name                  = (uint8_t *)s_adv_name;
    fields.name_len              = (uint8_t)strlen(s_adv_name);
    fields.name_is_complete      = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        esp_rom_printf("[blebridge] adv set_fields FAILED: %d\n", rc);
        return;
    }

    struct ble_gap_adv_params adv = {
        .conn_mode = BLE_GAP_CONN_MODE_NON,   /* broadcaster, no connections */
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        /* Slow advertising (~1 s) to minimise radio time stolen from the WiFi
         * mesh — fast default advertising starves the mesh keepalive and the
         * root drops the child every ~10 s.  Units of 0.625 ms. */
        .itvl_min  = 1600,
        .itvl_max  = 1600,
    };
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv, NULL, NULL);
    if (rc != 0)
        esp_rom_printf("[blebridge] adv start FAILED: %d\n", rc);
    else
        esp_rom_printf("[blebridge] advertising as '%s' (non-connectable)\n",
                       s_adv_name);
}

/* -------------------------------------------------------------------------
 * Passive scan of everything in range.
 * ------------------------------------------------------------------------- */
static volatile bool s_scanning;

static void start_scan(void)
{
    struct ble_gap_disc_params p = {
        /* Very low duty (~10%): unlike ESP-NOW, full WiFi-mesh must maintain an
         * association, and aggressive BLE scanning starves the radio so the
         * child misses mesh keepalives and the root drops it every ~10 s.
         * 30 ms window / 300 ms interval leaves the radio mostly to WiFi. */
        .itvl              = BLE_GAP_SCAN_ITVL_MS(300),
        .window            = BLE_GAP_SCAN_WIN_MS(30),
        .filter_policy     = BLE_HCI_SCAN_FILT_NO_WL,
        .limited           = 0,
        .passive           = 1,
        .filter_duplicates = 0,                          /* keep all sightings */
    };
    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &p,
                          scan_event_cb, NULL);
    if (rc != 0) {
        esp_rom_printf("[blebridge] ble_gap_disc FAILED: %d\n", rc);
    } else {
        s_scanning = true;
        esp_rom_printf("[blebridge] passive scan-all active\n");
    }
}

/* Guards BOTH scan and advertising -- the periodic status-refresh callout
 * (dbg_callout_fn) restarts advertising every 6 s independent of this flag,
 * so it must check s_paused too (see below) or advertising alone keeps the
 * radio busy through a "paused" window and the reconnect starves exactly
 * as before (observed on hardware: pausing only the scan was not enough,
 * advertising restarts continued right through every failed reconnect). */
static volatile bool s_paused;

void blebridge_pause_scan(void)
{
    if (!s_synced || s_paused)
        return;
    s_paused = true;
    if (s_scanning) {
        ble_gap_disc_cancel();
        s_scanning = false;
    }
    if (ble_gap_adv_active())
        ble_gap_adv_stop();
    esp_rom_printf("[blebridge] scan+adv paused (yielding radio to WiFi)\n");
}

void blebridge_resume_scan(void)
{
    if (!s_synced || !s_paused)
        return;
    s_paused = false;
    start_scan();
    start_advertising();
    esp_rom_printf("[blebridge] scan+adv resumed\n");
}

static void on_sync(void)
{
    int rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        esp_rom_printf("[blebridge] infer addr type FAILED: %d\n", rc);
        return;
    }
    esp_rom_printf("[blebridge] NimBLE ready (scan + advertise)\n");
    s_synced = true;
    start_advertising();   /* broadcaster */
    start_scan();          /* observer    */

    /* Drive the status-in-advert refresh from THIS (host) task. */
    ble_npl_callout_init(&s_dbg_callout, nimble_port_get_dflt_eventq(),
                         dbg_callout_fn, NULL);
    ble_npl_callout_reset(&s_dbg_callout, ble_npl_time_ms_to_ticks32(2000));
}

void blebridge_update_name(const char *name)
{
    if (!name || !*name || !s_synced)
        return;
    strncpy(s_adv_name, name, sizeof(s_adv_name) - 1);
    s_adv_name[sizeof(s_adv_name) - 1] = '\0';
    ble_gap_adv_stop();
    start_advertising();   /* re-advertise with the new name */
}

void blebridge_set_status(const char *s)
{
    if (!s || !*s) return;
    strncpy(s_status, s, sizeof(s_status) - 1);
    s_status[sizeof(s_status) - 1] = '\0';
}

/* Runs in the NimBLE host task: copy the status string into the advert name and
 * re-arm.  Decoupled from the caller so it keeps updating even if the caller is
 * blocked inside esp_mesh_send. */
static void dbg_callout_fn(struct ble_npl_event *ev)
{
    (void)ev;
    if (!s_paused) {
        strncpy(s_adv_name, s_status, sizeof(s_adv_name) - 1);
        s_adv_name[sizeof(s_adv_name) - 1] = '\0';
        ble_gap_adv_stop();
        start_advertising();
    }
    /* Keep re-arming even while paused so the status text catches up as
     * soon as blebridge_resume_scan() lifts the pause. */
    ble_npl_callout_reset(&s_dbg_callout, ble_npl_time_ms_to_ticks32(6000));
}

static void on_reset(int reason)
{
    esp_rom_printf("[blebridge] NimBLE reset reason=%d\n", reason);
}

static void nimble_host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void blebridge_start(const char *adv_name)
{
    if (adv_name && *adv_name) {
        strncpy(s_adv_name, adv_name, sizeof(s_adv_name) - 1);
        s_adv_name[sizeof(s_adv_name) - 1] = '\0';
    }

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        esp_rom_printf("[blebridge] nimble_port_init FAILED: %d\n", (int)err);
        return;
    }
    /* Guard the controller event dispatch (see shim_ble_patch.c). */
    shim_ble_patch_modules_funcs();

    /* No GAP service / GATT here — broadcaster+observer only.  The advertised
     * name is set directly in the adv payload (start_advertising). */
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb  = on_sync;

    nimble_port_freertos_init(nimble_host_task);
    esp_rom_printf("[blebridge] NimBLE host task launched\n");
}

unsigned blebridge_seen_count(void) { return s_seen; }
unsigned blebridge_fwd_count(void)  { return s_fwd;  }
