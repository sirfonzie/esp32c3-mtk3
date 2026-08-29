/*
 * udp_pingpong_c3.c -- lwIP UDP C3-to-C3 ping-pong, single firmware.
 *
 * Flash the same binary on two ESP32-C3 boards. Both connect to the same
 * WiFi AP (set WIFI_SSID / WIFI_PASSWORD below). Role is assigned by MAC
 * comparison: lower MAC = initiator (sends PING), higher = responder (echoes PONG).
 *
 * Discovery: both boards broadcast a DISC packet on UDP port 5554 until they
 * hear each other. Power both boards within ~30 s of each other.
 *
 * Initiator prints RTT for every round and summary stats every PROGRESS_EVERY
 * rounds. Press 'e' on either board to stop and print final stats.
 *
 * Build:  idf.py -C examples/udp_pingpong_c3 build
 * Flash:  same binary on both boards.
 */

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_mac.h"
#include "lwip/sockets.h"
#include "lwip/ip_addr.h"

#include <string.h>
#include <stdint.h>
#include "esp_rom_serial_output.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>

/* WiFi credentials — edit before building */
#define WIFI_SSID      "YOUR_SSID"
#define WIFI_PASSWORD  "YOUR_PASSWORD"

#define DISC_PORT        5554
#define PING_PORT        5555
#define DISC_INTERVAL_MS 200
#define DISC_OVERLAP_MS  2000   /* keep broadcasting after peer found so they find us too */
#define PING_INTERVAL_MS 2000
#define PONG_TIMEOUT_MS  1000
#define PROGRESS_EVERY   20

typedef struct {
    char    tag[4];
    uint8_t mac[6];
} __attribute__((packed)) disc_pkt_t;

typedef struct {
    char     tag[4];
    uint32_t seq;
} __attribute__((packed)) ping_pkt_t;

static volatile int        s_got_ip = 0;
static esp_netif_ip_info_t s_ip_info;
static uint8_t             s_own_mac[6];

/* -------------------------------------------------------------------------
 * WiFi
 * ------------------------------------------------------------------------- */
static void sta_event_handler(void *arg, esp_event_base_t base,
                              int32_t id, void *data)
{
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = data;
        s_ip_info = ev->ip_info;
        s_got_ip  = 1;
    }
}

static int wifi_connect(void)
{
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                               sta_event_handler, NULL);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
    esp_wifi_set_ps(WIFI_PS_NONE);

    wifi_config_t wcfg = {};
    strncpy((char *)wcfg.sta.ssid,     WIFI_SSID,     sizeof(wcfg.sta.ssid)     - 1);
    strncpy((char *)wcfg.sta.password, WIFI_PASSWORD, sizeof(wcfg.sta.password) - 1);
    wcfg.sta.pmf_cfg.capable  = true;
    wcfg.sta.pmf_cfg.required = false;
    esp_wifi_set_config(WIFI_IF_STA, &wcfg);

    tm_printf((UB *)"[udp] connecting to %s...\n", WIFI_SSID);
    for (int attempt = 0; attempt < 10 && !s_got_ip; attempt++) {
        esp_wifi_connect();
        for (int i = 0; i < 300 && !s_got_ip; i++)
            tk_dly_tsk(100);
        if (s_got_ip) break;
        tm_printf((UB *)"[udp] attempt %d failed, retrying...\n", attempt + 1);
        esp_wifi_disconnect();
        tk_dly_tsk(2000);
    }

    if (s_got_ip) {
        esp_wifi_get_mac(WIFI_IF_STA, s_own_mac);
        tm_printf((UB *)"[udp] IP=" IPSTR "  MAC=%02x:%02x:%02x:%02x:%02x:%02x\n",
                  IP2STR(&s_ip_info.ip),
                  s_own_mac[0], s_own_mac[1], s_own_mac[2],
                  s_own_mac[3], s_own_mac[4], s_own_mac[5]);
    }
    return s_got_ip ? 0 : -1;
}

static int check_key(void)
{
    uint8_t c;
    return (esp_rom_output_rx_one_char(&c) == 0) ? (int)c : -1;
}

static void log_rssi(void)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
        tm_printf((UB *)"[rssi] ch=%d  rssi=%d dBm\n", ap.primary, ap.rssi);
}

/* -------------------------------------------------------------------------
 * Discovery — both boards broadcast DISC on port 5554 until they hear each
 * other. Returns peer IP (network byte order), fills peer_mac.
 * ------------------------------------------------------------------------- */
static uint32_t run_discovery(uint8_t *peer_mac)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    int bcast = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &bcast, sizeof(bcast));

    struct timeval tv = { .tv_usec = DISC_INTERVAL_MS * 1000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in local = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port        = htons(DISC_PORT),
    };
    bind(sock, (struct sockaddr *)&local, sizeof(local));

    struct sockaddr_in bcast_addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_BROADCAST),
        .sin_port        = htons(DISC_PORT),
    };

    disc_pkt_t tx = { .tag = {'D','I','S','C'} };
    memcpy(tx.mac, s_own_mac, 6);

    tm_printf((UB *)"[udp] discovery — broadcasting on port %d\n", DISC_PORT);

    uint32_t peer_ip    = 0;
    UW       found_at   = 0;
    bool     in_overlap = false;

    for (;;) {
        sendto(sock, &tx, sizeof(tx), 0,
               (struct sockaddr *)&bcast_addr, sizeof(bcast_addr));

        disc_pkt_t rx;
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        int n = recvfrom(sock, &rx, sizeof(rx), 0,
                         (struct sockaddr *)&from, &fromlen);

        if (n == (int)sizeof(disc_pkt_t) &&
            memcmp(rx.tag, "DISC", 4) == 0 &&
            memcmp(rx.mac, s_own_mac, 6) != 0 && !peer_ip) {
            memcpy(peer_mac, rx.mac, 6);
            peer_ip    = from.sin_addr.s_addr;
            SYSTIM t;
            tk_get_otm(&t);
            found_at   = t.lo;
            in_overlap = true;
        }

        /* Keep broadcasting for DISC_OVERLAP_MS after finding the peer so the
         * other board is guaranteed to hear our DISC before we stop sending. */
        if (in_overlap) {
            SYSTIM now;
            tk_get_otm(&now);
            if (now.lo - found_at >= DISC_OVERLAP_MS)
                break;
        }
    }

    close(sock);

    tm_printf((UB *)"[udp] peer: " IPSTR "  MAC=%02x:%02x:%02x:%02x:%02x:%02x\n",
              IP2STR((ip4_addr_t *)&peer_ip),
              peer_mac[0], peer_mac[1], peer_mac[2],
              peer_mac[3], peer_mac[4], peer_mac[5]);
    return peer_ip;
}

/* -------------------------------------------------------------------------
 * Initiator (lower MAC) — sends PING, measures RTT
 * ------------------------------------------------------------------------- */
static void run_initiator(uint32_t peer_ip)
{
    tm_printf((UB *)"[udp] INITIATOR (lower MAC) — PING every %d ms\n",
              PING_INTERVAL_MS);

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    struct timeval tv = { .tv_usec = PONG_TIMEOUT_MS * 1000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in local = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port        = htons(0),
    };
    bind(sock, (struct sockaddr *)&local, sizeof(local));

    struct sockaddr_in peer = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = peer_ip,
        .sin_port        = htons(PING_PORT),
    };

    uint32_t rtt_min = UINT32_MAX, rtt_max = 0;
    uint64_t rtt_sum = 0;
    int ok = 0, lost = 0, round = 0;

    for (;;) {
        int ch = check_key();
        if (ch == 'e' || ch == 'E') break;

        round++;
        ping_pkt_t ping = { .tag = {'P','I','N','G'}, .seq = (uint32_t)round };

        SYSTIM t0, t1;
        tk_get_otm(&t0);
        sendto(sock, &ping, sizeof(ping), 0,
               (struct sockaddr *)&peer, sizeof(peer));

        ping_pkt_t pong;
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        int n = recvfrom(sock, &pong, sizeof(pong), 0,
                         (struct sockaddr *)&from, &fromlen);
        tk_get_otm(&t1);

        if (n == (int)sizeof(ping_pkt_t) && memcmp(pong.tag, "PONG", 4) == 0) {
            UW rtt = t1.lo - t0.lo;
            if (rtt < rtt_min) rtt_min = rtt;
            if (rtt > rtt_max) rtt_max = rtt;
            rtt_sum += rtt;
            ok++;
            tm_printf((UB *)"[udp] #%d  rtt=%u ms\n", round, (unsigned)rtt);
        } else {
            lost++;
            tm_printf((UB *)"[udp] #%d  TIMEOUT\n", round);
        }

        if (round % PROGRESS_EVERY == 0) {
            tm_printf((UB *)"[udp] --- round %d  ok=%d lost=%d"
                      "  rtt min=%u avg=%u max=%u ms ---\n",
                      round, ok, lost,
                      ok ? (unsigned)rtt_min : 0,
                      ok ? (unsigned)(rtt_sum / (uint64_t)ok) : 0,
                      (unsigned)rtt_max);
            log_rssi();
        }

        tk_dly_tsk(PING_INTERVAL_MS);
    }

    close(sock);
    tm_printf((UB *)"\n=== UDP ping-pong result ===\n");
    tm_printf((UB *)"  rounds: %d  ok: %d  lost: %d\n", round, ok, lost);
    if (ok > 0)
        tm_printf((UB *)"  rtt min=%u avg=%u max=%u ms\n",
                  (unsigned)rtt_min,
                  (unsigned)(rtt_sum / (uint64_t)ok),
                  (unsigned)rtt_max);
    tm_printf((UB *)"============================\n");
}

/* -------------------------------------------------------------------------
 * Responder (higher MAC) — echoes PING -> PONG
 * ------------------------------------------------------------------------- */
static void run_responder(void)
{
    tm_printf((UB *)"[udp] RESPONDER (higher MAC) — listening on port %d\n",
              PING_PORT);

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    struct timeval tv = { .tv_sec = 0, .tv_usec = 500000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in local = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port        = htons(PING_PORT),
    };
    bind(sock, (struct sockaddr *)&local, sizeof(local));

    int count = 0;
    for (;;) {
        int ch = check_key();
        if (ch == 'e' || ch == 'E') break;

        ping_pkt_t rx;
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        int n = recvfrom(sock, &rx, sizeof(rx), 0,
                         (struct sockaddr *)&from, &fromlen);

        if (n != (int)sizeof(ping_pkt_t) || memcmp(rx.tag, "PING", 4) != 0)
            continue;

        ping_pkt_t pong = { .tag = {'P','O','N','G'}, .seq = rx.seq };
        sendto(sock, &pong, sizeof(pong), 0,
               (struct sockaddr *)&from, fromlen);

        count++;
        tm_printf((UB *)"[udp] #%d  PONG sent\n", count);

        if (count % PROGRESS_EVERY == 0)
            log_rssi();
    }

    close(sock);
    tm_printf((UB *)"[udp] responder done. rounds served: %d\n", count);
}

/* -------------------------------------------------------------------------
 * Tasks
 * ------------------------------------------------------------------------- */
LOCAL void idle_task(INT s, void *e)
{
    (void)s; (void)e;
    for (;;) Asm("wfi" ::: "memory");
}

LOCAL void pingpong_task(INT s, void *e)
{
    (void)s; (void)e;

    if (wifi_connect() != 0) {
        tm_printf((UB *)"[udp] WiFi connect failed — check WIFI_SSID/WIFI_PASSWORD\n");
        goto done;
    }
    log_rssi();

    {
        uint8_t  peer_mac[6];
        uint32_t peer_ip = run_discovery(peer_mac);

        if (memcmp(s_own_mac, peer_mac, 6) < 0)
            run_initiator(peer_ip);
        else
            run_responder();
    }

done:
    esp_wifi_disconnect();
    tk_dly_tsk(500);
    tm_printf((UB *)"[udp] idle.\n");
    for (;;) tk_dly_tsk(5000);
}

EXPORT INT usermain(void)
{
    tm_printf((UB *)"[udp] MTK3 lwIP UDP C3-to-C3 ping-pong\n");

    T_CTSK idle = {
        .tskatr  = TA_HLNG | TA_RNG0,
        .task    = (FP)idle_task,
        .itskpri = CNF_MAX_TSKPRI,
        .stksz   = 256,
    };
    tk_sta_tsk(tk_cre_tsk(&idle), 0);

    T_CTSK ctsk = {
        .tskatr  = TA_HLNG | TA_RNG0,
        .task    = (FP)pingpong_task,
        .itskpri = 6,
        .stksz   = 8192,
    };
    tk_sta_tsk(tk_cre_tsk(&ctsk), 0);

    tk_slp_tsk(TMO_FEVR);
    return 0;
}
