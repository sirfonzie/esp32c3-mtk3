/*
 * main.c — ble_espmesh_demo: BLE (scan-all + advertise) coexisting with
 * ESP-MESH on the ESP32-C3 MTK3 port.  This is the "bed node" scenario:
 * a mains-powered node that joins the mesh as a child AND listens for BLE
 * advertisements, forwarding what it hears up the mesh to the root.
 *
 * Roles for the bring-up test:
 *   ROOT app (`ble_espmesh_root`) : mesh ROOT + BLE scan-all + BLE advertise
 *   this app                      : mesh NODE + BLE scan-all + BLE advertise
 *
 * Init order is coexistence-critical:
 *   1. espmesh_start(NODE)  — runs esp_wifi_init() (arms the RF coex context)
 *                             then joins the mesh.
 *   2. blebridge_start()    — NimBLE scan + advertise (controller needs the
 *                             coex context from step 1).
 *
 * The MTK3-on-IDF C3 boot shim is lifted verbatim from espmesh_demo.
 */
#include <string.h>
#include <stdio.h>           /* snprintf */
#include "esp_rom_sys.h"     /* esp_rom_printf */
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include "espmesh.h"
#include "blebridge.h"

/* --- MTK3-on-IDF C3 boot shim -------------------------------------------- */

/* Use the same central weak __wrap_esp_startup_start_app mechanism from
 * components/mtkernel:
 * (mtk_startup.c) handles the FreeRTOS bypass and hands off to knl_main().  We
 * only override it to enlarge the kernel-object heap — ESP-MESH needs more
 * semaphore/task control blocks than the 32 KB default (CNF_MAX_SEMID=128).
 * Unlike espmesh_demo we do NOT override esp_rom_output_tx_wait_idle and do NOT
 * busy-wait on the USB FIFO: with the BLE controller enabled those hung the C3
 * before usermain ever ran.  esp_rom_printf is the ROM-safe console path. */
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

/* --- application --------------------------------------------------------- */

/* Yield the radio to WiFi during a mesh reconnect; resume BLE scan once the
 * parent link is back.  See espmesh_set_link_change_cb's doc comment: BLE
 * scan competing with an in-flight auth/assoc handshake can make the coex
 * arbiter starve WiFi indefinitely. */
static void on_espmesh_link_change(bool connected)
{
    if (connected)
        blebridge_resume_scan();
    else
        blebridge_pause_scan();
}

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
    esp_rom_printf("[demo] ble_espmesh_demo: NODE + BLE scan/advertise\n");

    T_CTSK ctsk_idle = {
        .exinf = NULL, .tskatr = TA_HLNG | TA_RNG0,
        .itskpri = CNF_MAX_TSKPRI, .stksz = 1024,
        .task = (FP)task_idle,
    };
    tk_sta_tsk(tk_cre_tsk(&ctsk_idle), 0);


    /* 1. Join the mesh as a child.  This also runs esp_wifi_init(), which
     *    must precede the BLE controller for coexistence. */
    esp_err_t r = espmesh_start(ESPMESH_ROLE_NODE);
    esp_rom_printf("[demo] espmesh_start -> %d\n", (int)r);
    if (r != 0 /* ESP_OK */)
        goto idle;

    /* 2. Wait for mesh parent connection before starting BLE.  The mesh 4-way
     * handshake (WPA2 between child and parent mesh-AP) times out (reason=204)
     * when BLE scan is already running — the radio contention prevents the EAPOL
     * frames from completing within the handshake window.  Poll until
     * PARENT_CONNECTED fires (or 30 s timeout), then give the connection 2 s to
     * stabilize before handing the radio to BLE. */
    esp_rom_printf("[demo] waiting for mesh parent connection...\n");
    {
        int mtot, pconn, lastid, wconn, wdisc;
        for (int i = 0; i < 60; i++) {   /* up to 30 s (60 × 500 ms) */
            espmesh_dbg(&mtot, &pconn, &lastid, &wconn, &wdisc);
            int layer = espmesh_get_layer();
            esp_rom_printf("[demo] wait %d/60: pconn=%d layer=%d\n", i, pconn, layer);
            if (pconn > 0 || layer > 0) {
                esp_rom_printf("[demo] mesh up (pconn=%d layer=%d); stabilising 3 s\n",
                               pconn, layer);
                tk_dly_tsk(3000);
                break;
            }
            tk_dly_tsk(500);
        }
    }

    /* 3. Bring up BLE now that the mesh handshake has settled. */
    blebridge_start("MTK3-C3-BLE");
    esp_rom_printf("[demo] blebridge started; advertising 'MTK3-C3-BLE'\n");

    /* Registered only now (not before blebridge_start()): a disconnect
     * firing before BLE exists would call blebridge_resume_scan() on an
     * uninitialised NimBLE stack. */
    espmesh_set_link_change_cb(on_espmesh_link_change);

    int ok = 0;
    int acked = 0;
    for (;;) {
        int mtot, pconn, lastid, wconn, wdisc;
        espmesh_dbg(&mtot, &pconn, &lastid, &wconn, &wdisc);

        char hb[64];
        int n = snprintf(hb, sizeof(hb), "C3 hb p%d ok%d s%u",
                         pconn, ok, blebridge_seen_count());
        /* Gate on mesh layer > 0 to avoid queuing a stuck packet when the
         * parent route isn't established — a queued-but-undeliverable packet
         * causes xreq spam every ~110ms which blocks the mesh handshake.
         * Root address is looked up fresh every round (not hardcoded) so
         * this works with whichever board is currently the root. */
        int rc;
        uint8_t root[6];
        if (espmesh_is_connected() && espmesh_get_root_addr(root)) {
            rc = (int)espmesh_send(root, hb, n);
            if (rc == 0) ok++;
        } else {
            rc = 0x400b;
        }

        char st[28];
        snprintf(st, sizeof(st), "P%d ok%d r%x s%u",
                 pconn, ok, (unsigned)rc, blebridge_seen_count());
        blebridge_set_status(st);

        esp_rom_printf("[demo] %s rc=0x%x ok=%d\n", hb, (unsigned)rc, ok);

        /* Listen briefly for the root's relayed ACK (root->UDP responder
         * ->root->us).
         * Short timeout: this is a best-effort check, not a blocking wait —
         * the next heartbeat still fires on schedule either way. */
        if (rc == 0) {
            uint8_t from[6];
            char ackbuf[64];
            int alen = espmesh_recv(from, ackbuf, sizeof(ackbuf) - 1, 1500);
            if (alen > 0) {
                ackbuf[alen] = '\0';
                acked++;
                esp_rom_printf("[demo] ACK #%d from %02x:%02x:%02x:%02x:%02x:%02x: %s\n",
                              acked, from[0], from[1], from[2],
                              from[3], from[4], from[5], ackbuf);
            }
        }

        tk_dly_tsk(3500);   /* 1s hammered esp_mesh_send hangs the child TX */
    }

idle:
    for (;;)
        tk_dly_tsk(10000);
    return 0;
}
