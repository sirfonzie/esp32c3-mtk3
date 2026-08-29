/*
 * blebridge.h — BLE scan-all + advertise, bridged up an ESP-MESH uplink.
 *
 * Reusable component (NO uT-Kernel dependency — keep it that way so the
 * NimBLE BSD sys/queue.h is not shadowed by the kernel's sys/queue.h).
 * Runs alongside the espmesh component to exercise BLE + WiFi-mesh
 * coexistence on the ESP32-C3 MTK3 port (the "bed node" scenario).
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Bring up NimBLE: a passive scan of ALL advertisements in range plus a
 * continuous non-connectable advertisement as `adv_name`.  Each scanned
 * advert is forwarded (rate-limited) up the mesh to the root via
 * espmesh_send(), so the findings surface on the root's console.
 *
 * MUST be called AFTER espmesh_start() — esp_wifi_init() (done inside
 * espmesh_start) sets up the RF coexistence context the BLE controller needs.
 */
void blebridge_start(const char *adv_name);

/* Observability counters. */
unsigned blebridge_seen_count(void);   /* total adverts scanned        */
unsigned blebridge_fwd_count(void);    /* adverts forwarded up the mesh */

/* Replace the advertised name at runtime (e.g. to encode live status a phone
 * can read when the serial console isn't capturable).  Safe to call
 * periodically from another task; restarts advertising with the new name. */
void blebridge_update_name(const char *name);

/* Publish a free-form status string for the in-advert telemetry callout (runs
 * in the NimBLE host task).  Caller only writes the string — no NimBLE calls —
 * so the advert keeps refreshing even if the caller blocks (e.g. inside
 * esp_mesh_send).  Truncated to fit the advert (≤27 chars). */
void blebridge_set_status(const char *s);

/*
 * Pause/resume the passive scan (advertising is left running).  Intended for
 * callers that need to hand the radio fully to WiFi during a mesh
 * reconnect: BLE scan competing with an in-progress auth/assoc handshake
 * can make the coex arbiter starve the WiFi side indefinitely (observed on
 * hardware as a reconnect loop that never completes).  No-ops if BLE hasn't
 * synced yet or is already in the requested state.
 */
void blebridge_pause_scan(void);
void blebridge_resume_scan(void);

#ifdef __cplusplus
}
#endif
