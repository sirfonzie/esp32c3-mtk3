/*
 * espmesh.c — reusable ESP-MESH bring-up for uT-Kernel 3.0 on ESP32-C3.
 *
 * Milestone A: router-less fixed root + self-organized child.  This is the
 * known-good path distilled from the earlier examples/mesh_root and
 * examples/mesh_node experiments (see git tag pre-espmesh).  Milestone B —
 * getting the root to associate to a real router — is layered on top later.
 *
 * Logging uses esp_rom_printf so this component carries NO uT-Kernel
 * dependency and can be reused from a plain IDF app as well as an MTK3
 * usermain().  Keep it that way.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"   /* portMAX_DELAY, pdMS_TO_TICKS */
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mesh.h"
#include "esp_mac.h"
#include "esp_rom_sys.h"
#include "nvs_flash.h"
#include "espmesh.h"

/* All nodes in one mesh must share this ID.  Fixed for the experiment; promote
 * to Kconfig if multiple meshes ever need to coexist. */
static const uint8_t s_mesh_id[6] = {0x77, 0x77, 0x77, 0x77, 0x77, 0x77};

static volatile bool s_connected = false;   /* node: parent up / root: started */
static mesh_addr_t   s_root_addr;
static volatile bool s_have_root = false;
static esp_netif_t  *s_sta_netif;           /* root only: uplink to the router */
static espmesh_link_change_cb_t s_link_cb;

void espmesh_set_link_change_cb(espmesh_link_change_cb_t cb)
{
    s_link_cb = cb;
}

/* Debug event counters (surfaced via espmesh_dbg) to trace where the child
 * connect chain breaks when the console isn't capturable. */
static volatile int s_evt_mesh_total;       /* any MESH_EVENT delivered     */
static volatile int s_evt_parent_conn;      /* MESH_EVENT_PARENT_CONNECTED  */
static volatile int s_evt_mesh_last;        /* last MESH_EVENT id           */
static volatile int s_evt_wifi_staconn;     /* WIFI_EVENT_STA_CONNECTED     */
static volatile int s_evt_wifi_stadisc;     /* WIFI_EVENT_STA_DISCONNECTED  */

void espmesh_dbg(int *mtot, int *pconn, int *lastid, int *wconn, int *wdisc)
{
    if (mtot)   *mtot   = s_evt_mesh_total;
    if (pconn)  *pconn  = s_evt_parent_conn;
    if (lastid) *lastid = s_evt_mesh_last;
    if (wconn)  *wconn  = s_evt_wifi_staconn;
    if (wdisc)  *wdisc  = s_evt_wifi_stadisc;
}

int espmesh_get_layer(void)
{
    return esp_mesh_get_layer();
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)data;
    if (id == WIFI_EVENT_STA_CONNECTED)         s_evt_wifi_staconn++;
    else if (id == WIFI_EVENT_STA_DISCONNECTED) s_evt_wifi_stadisc++;
}

static void on_mesh_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    s_evt_mesh_total++;
    s_evt_mesh_last = (int)id;
    switch (id) {
    case MESH_EVENT_STARTED:
        esp_rom_printf("[espmesh] STARTED layer=%d\n", esp_mesh_get_layer());
        break;
    case MESH_EVENT_PARENT_CONNECTED:
        s_connected = true;
        s_evt_parent_conn++;
        esp_rom_printf("[espmesh] PARENT_CONNECTED layer=%d\n", esp_mesh_get_layer());
        /* Per esp_mesh.h: "once a device becomes a root, start DHCP client
         * immediately" -- only the root has a router uplink worth an IP;
         * mesh itself never starts this automatically. */
        if (esp_mesh_is_root() && s_sta_netif != NULL) {
            esp_err_t dr = esp_netif_dhcpc_start(s_sta_netif);
            esp_rom_printf("[espmesh] root: dhcpc_start -> 0x%x\n", (unsigned)dr);
        }
        if (s_link_cb) s_link_cb(true);
        break;
    case MESH_EVENT_PARENT_DISCONNECTED:
        s_connected = false;
        esp_rom_printf("[espmesh] PARENT_DISCONNECTED reason=%d\n",
                       ((mesh_event_disconnected_t *)data)->reason);
        if (s_link_cb) s_link_cb(false);
        break;
    case MESH_EVENT_CHILD_CONNECTED:
        esp_rom_printf("[espmesh] CHILD_CONNECTED\n");
        break;
    case MESH_EVENT_ROOT_ADDRESS:
        memcpy(s_root_addr.addr, ((mesh_event_root_address_t *)data)->addr, 6);
        s_have_root = true;
        esp_rom_printf("[espmesh] ROOT_ADDRESS captured\n");
        break;
    case MESH_EVENT_ROUTING_TABLE_ADD:
        esp_rom_printf("[espmesh] RT_ADD size=%d\n",
                       ((mesh_event_routing_table_change_t *)data)->rt_size_new);
        break;
    case MESH_EVENT_FIND_NETWORK:
        /* Child scanned and found the network (note: esp_mesh_connect() here
         * makes it give up with NO_PARENT_FOUND, so we don't force it). */
        esp_rom_printf("[espmesh] FIND_NETWORK\n");
        break;
    default:
        break;
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)data;
    if (id == IP_EVENT_STA_GOT_IP)
        esp_rom_printf("[espmesh] GOT IP (root has router uplink)\n");
}

static esp_err_t wifi_up(void)
{
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        r = nvs_flash_init();
    }
    if (r != ESP_OK) return r;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Only the root ever gets an IP (mesh non-root devices don't need lwIP
     * per esp_mesh.h), but the netif must exist before esp_wifi_init() for
     * the root's router association to have something to attach DHCP to
     * once MESH_EVENT_PARENT_CONNECTED confirms it's root. */
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* Modem-sleep drops the radio between DTIM beacons.  The mesh parent sends
     * the TX-window XON once after association; if it arrives during a sleep
     * window the child misses it and wnd stays 0 forever.  Keep radio always-on
     * for mesh nodes (all are mains-powered in this hospital scenario anyway). */
    esp_wifi_set_ps(WIFI_PS_NONE);
    return ESP_OK;
}

esp_err_t espmesh_start(espmesh_role_t role)
{
    esp_err_t r = wifi_up();
    if (r != ESP_OK) return r;

    ESP_ERROR_CHECK(esp_event_handler_register(MESH_EVENT, ESP_EVENT_ANY_ID,
                                               on_mesh_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               on_ip_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               on_wifi_event, NULL));

    ESP_ERROR_CHECK(esp_mesh_init());
    ESP_ERROR_CHECK(esp_mesh_set_max_layer(CONFIG_ESPMESH_MAX_LAYER));

    mesh_cfg_t cfg = MESH_INIT_CONFIG_DEFAULT();
    memcpy(cfg.mesh_id.addr, s_mesh_id, 6);
    cfg.channel = CONFIG_ESPMESH_CHANNEL;
    /* Follow the router through a CSA (channel switch) so the root keeps its
     * uplink instead of dropping the whole subtree.  Verified on HW: the root
     * rode the router from ch6 to ch3 with the child subtree intact, 53/53
     * sends OK over ~160 s (docs/ESPMESH_BRINGUP.md). */
    cfg.allow_channel_switch = true;
    memcpy(cfg.mesh_ap.password, CONFIG_ESPMESH_AP_PASSWORD,
           strlen(CONFIG_ESPMESH_AP_PASSWORD));
    cfg.mesh_ap.max_connection = 6;
    cfg.mesh_ap.nonmesh_max_connection = 0;

    /* Both roles carry the real router config: ESP-MESH (this IDF) does not
     * support a router-less startup — esp_mesh_start() rejects ssid_len=0 and
     * aborts.  We use the supported "fixed root" model: every node disables
     * root election (fix_root), the designated ROOT declares itself with
     * esp_mesh_set_type() after start and associates to the router; NODEs wait
     * for that fixed root and join it as children.  Exercising the ROOT path is
     * also Milestone B — the C3 root associating to the router. */
    memcpy(cfg.router.ssid, CONFIG_ESPMESH_ROUTER_SSID,
           strlen(CONFIG_ESPMESH_ROUTER_SSID));
    cfg.router.ssid_len = strlen(CONFIG_ESPMESH_ROUTER_SSID);
    memcpy(cfg.router.password, CONFIG_ESPMESH_ROUTER_PASSWORD,
           strlen(CONFIG_ESPMESH_ROUTER_PASSWORD));
    ESP_ERROR_CHECK(esp_mesh_set_config(&cfg));

    /* Fixed-root topology: nobody auto-elects; the root is designated below. */
    ESP_ERROR_CHECK(esp_mesh_fix_root(true));

    /* Allow many more scan attempts before giving up and going ROOTLESS.
     * Default is 10 (10 * ~300ms = 3s).  The root may take up to ~10s to
     * associate to the router and start advertising its mesh AP.  300 attempts
     * = ~90s of patience so the child doesn't give up before the root is up. */
    mesh_attempts_t attempts = {
        .scan        = 120,   /* up from default 10 */
        .vote        = 1,
        .fail        = 120,
        .monitor_ie  = 10,
    };
    esp_mesh_set_attempts(&attempts);

    ESP_ERROR_CHECK(esp_mesh_start());

    if (role == ESPMESH_ROLE_ROOT) {
        ESP_ERROR_CHECK(esp_mesh_set_type(MESH_ROOT));
        s_connected = true;
        esp_rom_printf("[espmesh] role=ROOT: declared root, associating to "
                       "router '%s'\n", CONFIG_ESPMESH_ROUTER_SSID);
    } else {
        esp_rom_printf("[espmesh] role=NODE: started, waiting for fixed root\n");
    }
    return ESP_OK;
}

bool espmesh_is_connected(void)
{
    if (s_connected) return true;
    /* Poll fallback: don't rely solely on the PARENT_CONNECTED event being
     * delivered to the app (it may not be on every port/coex config).  A
     * non-root node attached to a parent has layer > 0. */
    return esp_mesh_get_layer() > 0;
}

bool espmesh_get_root_addr(uint8_t out_mac[6])
{
    if (s_have_root) {
        memcpy(out_mac, s_root_addr.addr, 6);
        return true;
    }
    /* Event fallback: in a 2-layer tree the parent IS the root, so the parent
     * BSSID is the root's mesh address.  Use the live WiFi association directly
     * (esp_mesh_get_parent_bssid) rather than the mesh layer variable / the
     * ROOT_ADDRESS event — on the MTK3 child those app-facing state updates may
     * not arrive even though the association is up.  A non-zero parent BSSID
     * means we're associated.  (For deeper trees the ROOT_ADDRESS event path
     * above is required to reach the true root.) */
    mesh_addr_t parent;
    if (esp_mesh_get_parent_bssid(&parent) == ESP_OK) {
        if (parent.addr[0] | parent.addr[1] | parent.addr[2] |
            parent.addr[3] | parent.addr[4] | parent.addr[5]) {
            memcpy(out_mac, parent.addr, 6);
            return true;
        }
    }
    return false;
}

esp_err_t espmesh_send(const uint8_t dst_mac[6], const void *data, int len)
{
    mesh_addr_t dst;
    memcpy(dst.addr, dst_mac, 6);
    mesh_data_t d = {
        .data  = (uint8_t *)data,
        .size  = len,
        .proto = MESH_PROTO_BIN,
        .tos   = MESH_TOS_P2P,
    };
    return esp_mesh_send(&dst, &d, MESH_DATA_P2P, NULL, 0);
}

int espmesh_recv(uint8_t from_mac[6], void *buf, int buflen, int timeout_ms)
{
    mesh_addr_t from;
    mesh_data_t d = { .data = buf, .size = buflen };
    int flag = 0;
    int to = (timeout_ms < 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    esp_err_t r = esp_mesh_recv(&from, &d, to, &flag, NULL, 0);
    if (r != ESP_OK) return -1;
    if (from_mac) memcpy(from_mac, from.addr, 6);
    return d.size;
}
