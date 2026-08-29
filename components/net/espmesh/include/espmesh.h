/*
 * espmesh.h — reusable ESP-MESH stack for uT-Kernel 3.0 on ESP32-C3.
 *
 * Public API only; all WiFi/mesh/coex detail lives in espmesh.c so an
 * application (MTK3 usermain() or plain IDF app_main()) stays thin.
 *
 * Milestone A goal: bring a router-less fixed root + self-organized child mesh
 * up under MTK3 (reproduces the known-good path from the earlier mesh_root /
 * mesh_node experiments — see git tag pre-espmesh).
 */
#ifndef ESPMESH_H
#define ESPMESH_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ESPMESH_ROLE_ROOT = 0,   /* router-less fixed root, no external uplink   */
    ESPMESH_ROLE_NODE = 1,   /* self-organized child, joins root by mesh-ID  */
} espmesh_role_t;

/* Bring up WiFi + ESP-MESH for the given role.  Runs through esp_mesh_start();
 * returns ESP_OK once the mesh stack is running (a node is not necessarily
 * connected to a parent yet — poll espmesh_is_connected()). */
esp_err_t espmesh_start(espmesh_role_t role);

/* Node: true once a parent is connected.  Root: true once started. */
bool espmesh_is_connected(void);

/* Copy the root's mesh address (its STA MAC) into out_mac.
 * Returns false until MESH_EVENT_ROOT_ADDRESS has been observed. */
bool espmesh_get_root_addr(uint8_t out_mac[6]);

/* P2P send over the mesh to a specific node address (e.g. the root). */
esp_err_t espmesh_send(const uint8_t dst_mac[6], const void *data, int len);

/* Blocking receive.  timeout_ms < 0 waits forever.  Returns bytes received
 * (>0), or a negative value on timeout/error. */
int espmesh_recv(uint8_t from_mac[6], void *buf, int buflen, int timeout_ms);

/* Debug: event counters to trace where the child connect chain breaks.
 * Any pointer may be NULL. */
void espmesh_dbg(int *mesh_total, int *parent_conn, int *mesh_last_id,
                 int *wifi_sta_conn, int *wifi_sta_disc);

/* Wrapper around esp_mesh_get_layer() — avoids pulling esp_mesh.h (which
 * conflicts with MTK3 sysdef.h MSTATUS_* macros) into application code. */
int espmesh_get_layer(void);

/*
 * Fires on MESH_EVENT_PARENT_CONNECTED (connected=true) and
 * MESH_EVENT_PARENT_DISCONNECTED (connected=false), for either role (a
 * root's "parent" is its router uplink).  Intended so the application can
 * pause a concurrent BLE scan during a reconnect: BLE scanning while an
 * auth/assoc handshake is in flight can make the coex arbiter starve WiFi
 * indefinitely (observed on hardware as a reconnect loop that never
 * completes, "Coexist: Wi-Fi connect fail, apply reconnect coex policy").
 * Deliberately NOT wired to blebridge directly -- espmesh has no BLE
 * dependency; the application registers the callback after starting BLE.
 */
typedef void (*espmesh_link_change_cb_t)(bool connected);
void espmesh_set_link_change_cb(espmesh_link_change_cb_t cb);

#ifdef __cplusplus
}
#endif

#endif /* ESPMESH_H */
