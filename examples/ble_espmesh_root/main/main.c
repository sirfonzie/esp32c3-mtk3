/*
 * main.c — ble_espmesh_root: ESP-MESH ROOT + BLE scan/advertise, relaying
 * mesh P2P traffic to a plain UDP listener on the LAN and forwarding any
 * reply back down the mesh to the original sender.
 *
 * Roles:
 *   C3 (this app) : mesh ROOT (associates directly to the router) + BLE
 *                   scan-all + BLE advertise (dual-role, same as the node
 *                   in ble_espmesh_demo).
 *   UDP responder : optional host/tool on the same LAN, reachable via UDP
 *                   broadcast on RELAY_PORT. No discovery is needed, since a
 *                   broadcast's reply still carries the true unicast sender
 *                   address.
 *
 * Init order is coexistence-critical, mirroring the node:
 *   1. espmesh_start(ROOT) — associates directly to the router (runs
 *      esp_wifi_init(), arming the RF coex context) and starts advertising
 *      the mesh AP for children to join.
 *   2. Wait for our own router IP (own IP_EVENT_STA_GOT_IP handler) before
 *      starting BLE — the root's STA-to-router WPA2 handshake is subject to
 *      the same EAPOL-vs-BLE-scan radio contention as a child's parent
 *      handshake in ble_espmesh_demo.
 *   3. blebridge_start() — BLE scan + advertise once the radio has settled.
 *
 * Relay loop: block on espmesh_recv() for a P2P message from any child,
 * UDP-broadcast the payload on the LAN, wait briefly for a unicast reply,
 * and if one arrives, espmesh_send() it back to the child's mesh address.
 */
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include "esp_rom_sys.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "lwip/sockets.h"
#include "lwip/ip_addr.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include "espmesh.h"
#include "blebridge.h"

/* --- MTK3-on-IDF C3 boot shim (identical to ble_espmesh_demo) ------------ */
#define MTK_HEAP_BYTES (40 * 1024)
static uint8_t mtk_heap[MTK_HEAP_BYTES] __attribute__((aligned(8)));
extern void *knl_lowmem_top;
extern void *knl_lowmem_limit;
extern int   knl_main(void);

void __wrap_esp_startup_start_app(void)
{
    knl_lowmem_top   = mtk_heap;
    knl_lowmem_limit = mtk_heap + MTK_HEAP_BYTES;
    esp_rom_printf("\n[mtkernel] bypassing FreeRTOS; heap=%u KB\n",
                   (unsigned)(MTK_HEAP_BYTES / 1024));
    esp_rom_printf("[mtkernel] calling knl_main()\n");
    (void)knl_main();
    for (;;) {}
}

/* --- relay transport ------------------------------------------------------
 * UDP broadcast on RELAY_PORT; the listener's reply is unicast directly to
 * us (recvfrom on the broadcast socket yields the true sender address), so
 * no discovery handshake is needed on this leg. */
#define RELAY_PORT         5567
#define RELAY_TIMEOUT_MS   2000

static int relay_and_wait_ack(const char *msg, int len, char *ack, int acklen)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        esp_rom_printf("[demo] socket() failed: errno=%d\n", errno);
        return -1;
    }

    int bcast = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &bcast, sizeof(bcast));
    struct timeval tv = { .tv_sec  = RELAY_TIMEOUT_MS / 1000,
                          .tv_usec = (RELAY_TIMEOUT_MS % 1000) * 1000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in bcast_addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_BROADCAST),
        .sin_port        = htons(RELAY_PORT),
    };
    int sret = sendto(sock, msg, len, 0, (struct sockaddr *)&bcast_addr, sizeof(bcast_addr));
    if (sret < 0)
        esp_rom_printf("[demo] sendto() failed: errno=%d\n", errno);
    else
        esp_rom_printf("[demo] sendto() ok: %d bytes\n", sret);

    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);
    int n = recvfrom(sock, ack, acklen, 0, (struct sockaddr *)&from, &fromlen);
    close(sock);
    return n;   /* >0 bytes received, <=0 on timeout/error */
}

/* --- coexistence gate: wait for our own router IP before starting BLE --- */
static volatile bool s_got_ip;

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)data;
    if (id == IP_EVENT_STA_GOT_IP)
        s_got_ip = true;
}

/* Yield the radio to WiFi during a reconnect; resume BLE scan once the
 * router link is back.  See espmesh_set_link_change_cb's doc comment. */
static void on_espmesh_link_change(bool connected)
{
    if (connected)
        blebridge_resume_scan();
    else
        blebridge_pause_scan();
}

/* --- application ---------------------------------------------------------*/

/* Lowest-priority idle task.  The port requires one: if every other task is
 * blocked, knl_schedtsk would otherwise be NULL at the interrupt tail and the
 * dispatcher could not load a valid stack pointer.  Matches the pattern used
 * by the other examples. */
LOCAL void task_idle(INT stacd, void *exinf)
{
    (void)stacd; (void)exinf;
    for (;;) asm volatile ("wfi" ::: "memory");
}

EXPORT INT usermain(void)
{
    esp_rom_printf("[demo] ble_espmesh_root: ROOT + BLE scan/advertise + UDP relay\n");

    T_CTSK ctsk_idle = {
        .exinf = NULL, .tskatr = TA_HLNG | TA_RNG0,
        .itskpri = CNF_MAX_TSKPRI, .stksz = 1024,
        .task = (FP)task_idle,
    };
    tk_sta_tsk(tk_cre_tsk(&ctsk_idle), 0);


    /* 1. Declare ourselves root and associate to the router.  This also
     * runs esp_wifi_init(), which must precede the BLE controller. */
    esp_err_t r = espmesh_start(ESPMESH_ROLE_ROOT);
    esp_rom_printf("[demo] espmesh_start(ROOT) -> %d\n", (int)r);
    if (r != 0 /* ESP_OK */)
        goto idle;

    /* 2. Wait for our own STA association + DHCP before handing the radio
     * to BLE (same EAPOL-vs-scan risk the node's parent handshake has). */
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event, NULL);
    esp_rom_printf("[demo] waiting for router IP (own STA association)...\n");
    for (int i = 0; i < 60 && !s_got_ip; i++)   /* up to 30 s */
        tk_dly_tsk(500);

    if (s_got_ip) {
        esp_rom_printf("[demo] router IP up; stabilising 3 s\n");
        tk_dly_tsk(3000);
    } else {
        esp_rom_printf("[demo] no IP after 30 s; starting BLE anyway\n");
    }

    /* 3. Bring up BLE now that our own radio/association has settled. */
    blebridge_start("MTK3-C3-ROOT-BLE");
    esp_rom_printf("[demo] blebridge started; advertising 'MTK3-C3-ROOT-BLE'\n");

    /* Registered only now (not before blebridge_start()): a disconnect
     * firing before BLE exists would call blebridge_resume_scan() on an
     * uninitialised NimBLE stack.  From here on, any reconnect pauses BLE
     * scan for the duration -- see espmesh_set_link_change_cb's doc
     * comment for why (coex arbiter starving the WiFi handshake). */
    espmesh_set_link_change_cb(on_espmesh_link_change);

    /* Our own STA MAC -- needed below to break a feedback loop (see the
     * send-back-down-mesh comment).  Deliberately NOT espmesh_get_root_addr()
     * here: its fallback path (esp_mesh_get_parent_bssid) is a "find MY
     * parent" query that only makes sense for a non-root node, and whether
     * MESH_EVENT_ROOT_ADDRESS also fires locally on the root itself is not
     * guaranteed -- on hardware this left my_addr all-zero, so the memcmp
     * below never matched and every relay kept echoing (observed as nested
     * acknowledgements that never stopped).
     * esp_wifi_get_mac() is synchronous and always correct. */
    uint8_t my_addr[6] = {0};
    esp_wifi_get_mac(WIFI_IF_STA, my_addr);

    /* 4. Relay loop: anything a child sends us goes out to the LAN; any
     * reply goes back down the mesh to that same child. */
    int relayed = 0, acked = 0;
    for (;;) {
        uint8_t from[6];
        char msg[64];
        int n = espmesh_recv(from, msg, sizeof(msg) - 1, 5000);
        if (n <= 0)
            continue;   /* timeout; keep listening */
        msg[n] = '\0';
        relayed++;
        esp_rom_printf("[demo] mesh<-%02x:%02x:%02x:%02x:%02x:%02x: %s (relaying)\n",
                      from[0], from[1], from[2], from[3], from[4], from[5], msg);

        char ack[64];
        int alen = relay_and_wait_ack(msg, n, ack, sizeof(ack) - 1);
        if (alen > 0) {
            ack[alen] = '\0';
            acked++;
            /* blebridge's "forward up to root" resolves to OUR OWN address
             * when it's our own scan doing the forwarding (we ARE the
             * root) -- espmesh_send()'ing the ack back to ourselves would
             * feed it straight back into this same recv loop as if it were
             * a new message from a child, forming an unbounded echo
             * (observed on hardware: buffer content growing until it
             * truncated at the 64-byte limit). There is no child to deliver
             * to here, so just log it. */
            if (memcmp(from, my_addr, 6) == 0) {
                esp_rom_printf("[demo] udp ack: %s -- self-originated, not echoing back\n", ack);
            } else {
                esp_rom_printf("[demo] udp ack: %s -- relaying back down mesh\n", ack);
                espmesh_send(from, ack, alen);
            }
        } else {
            esp_rom_printf("[demo] no udp ack (timeout)\n");
        }

        char st[28];
        snprintf(st, sizeof(st), "rly%d ack%d s%u",
                 relayed, acked, blebridge_seen_count());
        blebridge_set_status(st);
    }

idle:
    for (;;)
        tk_dly_tsk(10000);
    return 0;
}
