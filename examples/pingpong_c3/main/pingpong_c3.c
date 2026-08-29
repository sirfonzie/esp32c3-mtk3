/*
 * pingpong_c3.c -- ESP-NOW C3-to-C3 ping-pong, single firmware.
 *
 * Flash the same binary on two ESP32-C3 boards. On boot each board
 * broadcasts its MAC (PKT_DISC) until it hears the peer. Role is then
 * assigned by MAC comparison: lower MAC = initiator, higher = responder.
 *
 * If the second board powers on late and misses the DISC phase it will
 * auto-discover from the first incoming PING packet.
 *
 *   Initiator: sends PING every PING_INTERVAL_MS, prints RTT.
 *   Responder: echoes PING -> PONG immediately, prints each echo.
 *
 * Build:  idf.py -C examples/pingpong_c3 build
 * Flash:  idf.py -C examples/pingpong_c3 -p /dev/ttyACM0 flash monitor
 *         (repeat with the second board on its port)
 */

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_now.h"
#include "esp_mac.h"

#include <string.h>
#include <tk/tkernel.h>
#include <tm/tmonitor.h>

#define ESPNOW_CHANNEL    1
#define DISC_INTERVAL_MS  200
#define PING_INTERVAL_MS  2000
#define PONG_TIMEOUT_MS   1000

#define PKT_DISC  0x01
#define PKT_PING  0x02
#define PKT_PONG  0x03

typedef struct { uint8_t type; uint8_t mac[6]; } __attribute__((packed)) disc_pkt_t;
typedef struct { uint8_t type; uint32_t seq;   } __attribute__((packed)) ping_pkt_t;

static const uint8_t BROADCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static uint8_t           s_own_mac[6];
static uint8_t           s_peer_mac[6];
static volatile bool     s_peer_found = false;
static volatile bool     s_pong_rx    = false;
static volatile uint32_t s_pong_seq   = 0;
static volatile bool     s_ping_rx    = false;
static volatile uint32_t s_ping_seq   = 0;
static volatile uint8_t  s_ping_src[6];

static void add_peer_if_new(const uint8_t *mac)
{
    if (esp_now_is_peer_exist(mac))
        return;
    esp_now_peer_info_t p = {};
    memcpy(p.peer_addr, mac, 6);
    p.channel = ESPNOW_CHANNEL;
    p.encrypt = false;
    esp_now_add_peer(&p);
}

static void send_cb(const esp_now_send_info_t *info, esp_now_send_status_t status)
{
    (void)info;
    if (status != ESP_NOW_SEND_SUCCESS)
        esp_rom_printf("[pp] TX FAIL\n");
}

static void recv_cb(const esp_now_recv_info_t *info,
                    const uint8_t *data, int len)
{
    if (len < 1)
        return;
    switch (data[0]) {

    case PKT_DISC:
        if (len >= (int)sizeof(disc_pkt_t) && !s_peer_found) {
            memcpy(s_peer_mac, data + 1, 6);
            add_peer_if_new(s_peer_mac);
            s_peer_found = true;
        }
        break;

    case PKT_PING:
        if (len >= (int)sizeof(ping_pkt_t)) {
            if (!s_peer_found) {
                /* late boot: discover peer from first incoming PING */
                memcpy(s_peer_mac, info->src_addr, 6);
                add_peer_if_new(s_peer_mac);
                s_peer_found = true;
            }
            memcpy((void *)s_ping_src, info->src_addr, 6);
            s_ping_seq = ((const ping_pkt_t *)data)->seq;
            s_ping_rx  = true;
        }
        break;

    case PKT_PONG:
        if (len >= (int)sizeof(ping_pkt_t)) {
            s_pong_seq = ((const ping_pkt_t *)data)->seq;
            s_pong_rx  = true;
        }
        break;
    }
}

static void wifi_espnow_init(void)
{
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
    esp_wifi_get_mac(WIFI_IF_STA, s_own_mac);

    esp_now_init();
    esp_now_register_send_cb(send_cb);
    esp_now_register_recv_cb(recv_cb);
    add_peer_if_new(BROADCAST);
}

static void run_discovery(void)
{
    disc_pkt_t pkt = { .type = PKT_DISC };
    memcpy(pkt.mac, s_own_mac, 6);

    tm_printf((UB *)"[pp] discovery — broadcasting every %d ms\n", DISC_INTERVAL_MS);
    while (!s_peer_found) {
        esp_now_send(BROADCAST, (uint8_t *)&pkt, sizeof(pkt));
        tk_dly_tsk(DISC_INTERVAL_MS);
    }
    tm_printf((UB *)"[pp] peer: %02x:%02x:%02x:%02x:%02x:%02x\n",
              s_peer_mac[0], s_peer_mac[1], s_peer_mac[2],
              s_peer_mac[3], s_peer_mac[4], s_peer_mac[5]);
}

static void run_initiator(void)
{
    tm_printf((UB *)"[pp] INITIATOR (lower MAC) — PING every %d ms\n",
              PING_INTERVAL_MS);
    uint32_t seq = 0;
    for (;;) {
        seq++;
        s_pong_rx = false;

        SYSTIM t0, t1;
        tk_get_otm(&t0);

        ping_pkt_t ping = { .type = PKT_PING, .seq = seq };
        esp_now_send(s_peer_mac, (uint8_t *)&ping, sizeof(ping));

        for (int i = 0; i < PONG_TIMEOUT_MS / 10 && !s_pong_rx; i++)
            tk_dly_tsk(10);

        tk_get_otm(&t1);
        UW rtt = t1.lo - t0.lo;

        if (s_pong_rx && s_pong_seq == seq)
            tm_printf((UB *)"[pp] #%lu  rtt=%u ms\n",
                      (unsigned long)seq, (unsigned)rtt);
        else
            tm_printf((UB *)"[pp] #%lu  TIMEOUT (%u ms)\n",
                      (unsigned long)seq, (unsigned)rtt);

        tk_dly_tsk(PING_INTERVAL_MS);
    }
}

static void run_responder(void)
{
    tm_printf((UB *)"[pp] RESPONDER (higher MAC) — waiting for PING\n");
    for (;;) {
        while (!s_ping_rx)
            tk_dly_tsk(5);
        s_ping_rx = false;

        uint32_t seq = s_ping_seq;
        uint8_t  src[6];
        memcpy(src, (void *)s_ping_src, 6);

        ping_pkt_t pong = { .type = PKT_PONG, .seq = seq };
        esp_now_send(src, (uint8_t *)&pong, sizeof(pong));

        tm_printf((UB *)"[pp] #%lu  PONG sent\n", (unsigned long)seq);
    }
}

LOCAL void idle_task(INT s, void *e)
{
    (void)s; (void)e;
    for (;;) Asm("wfi" ::: "memory");
}

LOCAL void pingpong_task(INT s, void *e)
{
    (void)s; (void)e;

    wifi_espnow_init();

    tm_printf((UB *)"[pp] MAC: %02x:%02x:%02x:%02x:%02x:%02x  ch=%d\n",
              s_own_mac[0], s_own_mac[1], s_own_mac[2],
              s_own_mac[3], s_own_mac[4], s_own_mac[5], ESPNOW_CHANNEL);

    run_discovery();

    if (memcmp(s_own_mac, s_peer_mac, 6) < 0)
        run_initiator();
    else
        run_responder();
}

EXPORT INT usermain(void)
{
    tm_printf((UB *)"[pp] MTK3 ESP-NOW C3-to-C3 ping-pong\n");

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
