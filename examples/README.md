# Examples

Standalone IDF apps that exercise one feature of the μT-Kernel 3.0 ESP32-C3 port each.

Every example links the shared kernel component at `../../components/mtkernel`, so the kernel itself is built once and reused. The `--wrap=esp_startup_start_app` / `--wrap=rtos_int_exit` link options and the weak default startup (32 KB heap, calls `knl_main()`) come from that component, so each example's `main.c` is just the demo content plus the idle task - no boot boilerplate.

## Build & run

From the repo root:

```sh
. ~/esp/esp-idf/export.sh
idf.py -C examples/<name> build
idf.py -C examples/<name> -p /dev/ttyACM0 flash monitor
```

(Set `set-target esp32c3` if your IDF default is not C3 yet.) The documented
examples target ESP32-C3 through the `iote_esp32c3_mini` or `iote_m5stamp_c3`
board kit.

Those commands use the default Super Mini/native USB-Serial-JTAG board kit. For
M5StampC3 examples, pass the M5 board selection and keep the example in its own
build/sdkconfig directory. The example's local `sdkconfig.defaults` must stay
first so feature-specific settings such as WiFi, BLE, custom partitions, or
watchdog timeouts are preserved; the root M5 defaults are listed second so the
console is switched to UART0/CH9102:

```sh
idf.py -C examples/<name> -B build_<name>_m5stamp_c3 \
  -DMTK_BOARD=iote_m5stamp_c3 \
  -DSDKCONFIG=../../build_<name>_m5stamp_c3/sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;../../sdkconfig.defaults.m5stamp_c3" \
  -p /dev/ttyACM0 build flash monitor
```

### Configuration differences between examples

All kernel-only examples share one baseline `sdkconfig.defaults` (`CONFIG_IDF_TARGET=esp32c3`, 4096 B main-task stack, 4 MB flash, USB-Serial-JTAG console, and `CONFIG_ESP_TASK_WDT_EN=n` - the Task-WDT's FreeRTOS idle-task subscription model has no analog on this port, so it is declared off rather than left silently inert). Where an example's config diverges from that baseline:

| Example | What's different, and why |
|---|---|
| `pingpong_c3/`, `udp_pingpong_c3/` | `CONFIG_LWIP_ENABLE=y`, `CONFIG_BT_ENABLED=n`, `CONFIG_FREERTOS_HZ=1000` (WiFi needs sub-10ms internal timeouts) - WiFi stack on, BLE off. |
| `ble_beacon/`, `ble_gatt/` | `CONFIG_MTK3_KERNEL_BLE=y`, `CONFIG_BT_NIMBLE_ROLE_{PERIPHERAL,OBSERVER,BROADCASTER,CENTRAL}=y` (all four NimBLE roles enabled), `CONFIG_BT_BLE_CCA_MODE_HW=y`. `CONFIG_ESP_COEX_SW_COEXIST_ENABLE` is left at its default (off) - these two never run WiFi, so there's nothing to coexist with. |
| `ble_espmesh_demo/`, `ble_espmesh_root/` | The one config combination that runs **both radios concurrently**: `CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y` is **required** here (it's off in `ble_beacon`/`ble_gatt` because they're BLE-only). NimBLE roles are narrowed to `OBSERVER`+`BROADCASTER` only (`PERIPHERAL`/`CENTRAL`=n) - this is scan-and-advertise, not a connectable GATT server. `CONFIG_LWIP_ENABLE=y` plus a **custom `partitions.csv`** (BLE + NimBLE + ESP-MESH + lwIP together exceed the default 1 MB app partition - these give the app ~2 MB). `CONFIG_ESPMESH_CHANNEL=10` is a Kconfig default in `components/net/espmesh/Kconfig`, editable via `idf.py menuconfig` → "MTK3 ESP-MESH" - **it must match your router's actual WiFi channel** (see the ESP-MESH section below). |

## Build & Flash Quick Reference

Use `/dev/ttyACM0` as a placeholder; on a multi-board setup, pass the port that
belongs to the board you are flashing.

| Project | Super Mini/native USB command | M5StampC3 command |
|---|---|---|
| `tasks/` | `idf.py -C examples/tasks -p /dev/ttyACM0 build flash monitor` | `idf.py -C examples/tasks -B build_tasks_m5stamp_c3 -DMTK_BOARD=iote_m5stamp_c3 -DSDKCONFIG=../../build_tasks_m5stamp_c3/sdkconfig -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;../../sdkconfig.defaults.m5stamp_c3" -p /dev/ttyACM0 build flash monitor` |
| `four_tasks/` | `idf.py -C examples/four_tasks -p /dev/ttyACM0 build flash monitor` | Same M5 pattern with `examples/four_tasks` |
| `uart_loopback/` | `idf.py -C examples/uart_loopback -p /dev/ttyACM0 build flash monitor` | Not recommended on M5StampC3: this example exercises DEV_SER UART1, while the M5 board kit deliberately leaves GPIO21/GPIO20 owned by UART0/CH9102 and does not register DEV_SER. |
| `adc/` | `idf.py -C examples/adc -p /dev/ttyACM0 build flash monitor` | Same M5 pattern with `examples/adc` |
| `i2c_scan/` | `idf.py -C examples/i2c_scan -p /dev/ttyACM0 build flash monitor` | Same M5 pattern with `examples/i2c_scan` |
| `pwm/` | `idf.py -C examples/pwm -p /dev/ttyACM0 build flash monitor` | Same M5 pattern with `examples/pwm` |
| `pingpong_c3/` | `idf.py -C examples/pingpong_c3 -p /dev/ttyACM0 build flash monitor` | Same M5 pattern with `examples/pingpong_c3` |
| `udp_pingpong_c3/` | `idf.py -C examples/udp_pingpong_c3 -p /dev/ttyACM0 build flash monitor` | Same M5 pattern with `examples/udp_pingpong_c3` |
| `ble_beacon/` | `idf.py -C examples/ble_beacon -p /dev/ttyACM0 build flash monitor` | Same M5 pattern with `examples/ble_beacon` |
| `ble_gatt/` | `idf.py -C examples/ble_gatt -p /dev/ttyACM0 build flash monitor` | Same M5 pattern with `examples/ble_gatt` |
| `ble_espmesh_root/` | `idf.py -C examples/ble_espmesh_root -p /dev/ttyACM0 build flash monitor` | Same M5 pattern with `examples/ble_espmesh_root` |
| `ble_espmesh_demo/` | `idf.py -C examples/ble_espmesh_demo -p /dev/ttyACM0 build flash monitor` | Same M5 pattern with `examples/ble_espmesh_demo` |

### WSL2 note

The ESP32-C3's USB-Serial-JTAG peripheral does not respond to `select()` write-readiness checks on WSL2, causing pyserial's `write()` to time out. Most example directories contain an `esptool.cfg` that sets `serial_write_timeout = 0`, switching pyserial to non-blocking `os.write()` and bypassing the broken `select()` path. If flashing times out in an example that does not have that file, add the same `esptool.cfg` to that example directory.

## What each example covers

### Kernel only - no radio required

| Path | Demonstrates | External hardware |
|---|---|---|
| `tasks/` | Preemptive multitasking - two equal-priority tasks alternating via `tk_dly_tsk`, plus the mandatory idle task. The smallest "real µT-Kernel app" template. | None |
| `four_tasks/` | Four tasks at different priorities (3–6) and delays (1–5 s), each printing its own counter. Demonstrates priority-preemptive scheduling with live timestamps. | None |
| `uart_loopback/` | `DEV_SER` device driver on UART1 - `tk_swri_dev` then `tk_srea_dev` in one task (two tasks deadlock on the mSDI per-device mutex). With a TX-RX jumper each `[tx] sent X` is followed by `[rx] got 0x..`; without it only the `[tx]` lines appear, which is a quick continuity check. | Jumper wire GPIO21 - GPIO20 |
| `adc/` | `DEV_ADC` polling-based one-shot reads of all 5 ADC1 channels (GPIO0..GPIO4). | None |
| `i2c_scan/` | Walks the 7-bit I²C address range (0x08–0x77) and reports which addresses ACK. SDA on GPIO6, SCL on GPIO7. | I²C device + pull-ups |
| `pwm/` | LEDC PWM on GPIO10, stepping duty cycle 0→25→50→75→100 % one step per second at 1 kHz. | LED + ~470 Ω resistor |

### WiFi - requires C3 board with WiFi antenna

Each WiFi example has its own `sdkconfig.defaults` - no extra build flags needed.

| Path | Demonstrates | External hardware |
|---|---|---|
| `pingpong_c3/` | ESP-NOW C3-to-C3 ping-pong. Flash the same binary on two C3 boards; role (initiator/responder) is auto-assigned by MAC address at boot. | Two ESP32-C3 boards |
| `udp_pingpong_c3/` | lwIP UDP C3-to-C3 ping-pong over a WiFi router. Flash the same binary on two C3 boards; role auto-assigned by MAC. Edit `WIFI_SSID` and `WIFI_PASSWORD` at the top of `main/udp_pingpong_c3.c` before building. Press `e` to stop and print final RTT stats. | Two ESP32-C3 boards + WiFi router |

### BLE - requires C3 board with BLE antenna

Each BLE example has its own `sdkconfig.defaults` - no extra build flags needed.

| Path | Demonstrates | How to verify |
|---|---|---|
| `ble_beacon/` | Non-connectable broadcaster. Advertises as `MTK3-Beacon` every 100 ms with manufacturer-specific data (`0xFFFF` + `MTK3` magic bytes) and TX power. | nRF Connect → Scanner: scan and see `MTK3-Beacon` with RSSI and raw manufacturer bytes |
| `ble_gatt/` | Connectable GATT server advertising as `MTK3-GATT`. Exposes two services: **Device Information** (0x180A - firmware/hardware/software revision strings) and a custom **MTK3 Stats** service (0xFF00) with a readable+notifiable status characteristic (heap + uptime, updates every 3 s) and a read+write echo characteristic. | nRF Connect → Scanner → Connect → Client tab: read 0xFF01, subscribe for notifications, write and read back 0xFF02 |

### WiFi + BLE concurrent (ESP-MESH) - requires two ESP32-C3 boards

| Path | Demonstrates | External hardware |
|---|---|---|
| `ble_espmesh_demo/` | ESP-MESH child ("NODE") **+** BLE scan/advertise running concurrently. Joins the mesh, then scans for ambient BLE and relays sightings to the root over the mesh. | One ESP32-C3 (see the full test below) |
| `ble_espmesh_root/` | ESP-MESH root **+** BLE scan/advertise, associating directly to your router and relaying anything received from mesh children out to the LAN over UDP broadcast. | One ESP32-C3 + WiFi router |

This pair is the port's most demanding in-repo concurrency test: two radios,
two mesh roles, and two ESP32-C3 boards. UDP relay receivers or other companion
tools are intentionally outside this repo.

If you want the node to print `ACK #N ...`, provide your own UDP responder on
the same LAN as `ble_espmesh_root`. The responder contract is:

- Bind a UDP socket to port `5567`.
- Receive broadcast datagrams sent by the root.
- Send a UDP reply back to the sender IP address and sender source port from
  the received packet. Do not assume the reply destination port is `5567`.
- Keep replies short; the root relays up to 63 bytes back down the mesh.

The payload format is plain text today. The root relays whatever bytes it
receives from the mesh and sends any non-empty UDP response back to the original
mesh child.

---

## BLE + ESP-MESH: architecture, bugs found, and how to run the full test

### What it proves

Both `ble_espmesh_demo` (NODE) and `ble_espmesh_root` (ROOT) run **BLE scan+advertise and ESP-MESH WiFi simultaneously** on the same single-core chip - the two components (`components/net/espmesh/`, `components/net/blebridge/`) are reusable and kernel-agnostic (no µT-Kernel dependency), wired together only at the application layer in each example's `main.c`.

```
[phone: any BLE advertiser]  --BLE adv-->  [C3 #1: NODE]  --mesh P2P-->  [C3 #2: ROOT]
                                                ^                              |
                                                |________ mesh P2P ack _______+
```

- **NODE** (`ble_espmesh_demo`) joins the mesh as a child of a fixed root, then scans for BLE advertisements and forwards sightings to the root over `espmesh_send()`.
- **ROOT** (`ble_espmesh_root`) associates directly to your router, accepts NODE as a mesh child, and for every mesh message it receives, relays the payload out as a UDP broadcast on the LAN. If an external UDP responder replies, the root relays that reply back down the mesh to whichever child sent the original message. It also runs its own BLE scan concurrently (dual-role, same as the NODE).

### Root election is NOT automatic

ESP-MESH natively supports self-organizing root election, but this component deliberately opts out via `esp_mesh_fix_root(true)` - the app declares its role explicitly by calling `espmesh_start(ESPMESH_ROLE_ROOT)` or `espmesh_start(ESPMESH_ROLE_NODE)`. This is why there are **two separate example binaries** rather than one that elects a root automatically. (The original reason: `esp_mesh_start()` rejects a fully router-less, auto-electing startup outright - fixed-root sidesteps that.)

### Coexistence ordering rule (the recurring theme of every bug below)

**WiFi/mesh must be fully settled before BLE starts, and BLE must yield the radio back during any WiFi reconnect.** Both examples follow this sequence:

1. `espmesh_start(ROLE)` - this internally calls `esp_wifi_init()`, arming the RF coexistence context BLE needs.
2. Wait for the WiFi side to actually settle (NODE: poll for `PARENT_CONNECTED`; ROOT: wait for its own router IP), then a few seconds of stabilization.
3. Only then call `blebridge_start()`.
4. Register `espmesh_set_link_change_cb()` so that if the WiFi link **ever drops and reconnects**, BLE scan/advertise pauses for the duration and resumes once reconnected.

Skipping any of these steps reproduces one of the three bugs below.

### Three bugs found bringing this test up (all fixed)

1. **Root never got an IP.** `esp_mesh.h` documents that an application must explicitly start a DHCP client once a device becomes root - ESP-MESH does not do this automatically, and the port's `espmesh.c` didn't either. The root associated at the WiFi layer but had no IP-capable interface, so anything the app tried to send over a plain socket went nowhere, silently. Fixed: `espmesh.c` now creates a STA netif and starts DHCP on `MESH_EVENT_PARENT_CONNECTED` when `esp_mesh_is_root()`.
2. **Self-echo feedback loop.** `blebridge`'s "forward this BLE sighting up to the root" resolves to a board's *own* address when that board *is* the root, so the ROOT could send an acknowledgement back to itself and treat that as a new message. Fixed in the application (`ble_espmesh_root/main.c`): capture the ROOT's own address via `esp_wifi_get_mac()` (not `espmesh_get_root_addr()` - that function's fallback path is a "find my parent" query that doesn't apply to a root) and skip the echo when the sender matches.
3. **BLE scan/advertising starving a WiFi reconnect.** If the mesh link drops for any reason (normal WiFi flakiness) while BLE is already running, the coex arbiter can starve the WiFi reconnect handshake indefinitely (`Coexist: Wi-Fi connect fail, apply reconnect coex policy`, repeating past the configured retry limit). This hits **both** BLE scanning and the periodic advertising restart (the status-refresh callout) - pausing only the scan wasn't sufficient; both needed to stop. Fixed: `espmesh_set_link_change_cb()` + `blebridge_pause_scan()`/`resume_scan()` (which now also stops/restarts advertising) - see the coexistence ordering rule above.

### Running the full two-board test

Recommended boot order - **ROOT → NODE** - so the node finds an already-live
fixed root instead of timing out and retrying:

```sh
# 1. ROOT: CONFIG_ESPMESH_ROUTER_SSID/PASSWORD default via components/net/espmesh/Kconfig
#    (override with `idf.py -C examples/ble_espmesh_root menuconfig` if your router differs)
idf.py -C examples/ble_espmesh_root -p /dev/ttyACM0 build flash monitor

# 2. NODE
idf.py -C examples/ble_espmesh_demo -p /dev/ttyACM1 build flash monitor
```

What healthy output looks like:

- **ROOT**: `[espmesh] PARENT_CONNECTED layer=1` → `[espmesh] root: dhcpc_start -> 0x0` → `[espmesh] GOT IP` → `[espmesh] CHILD_CONNECTED` (once NODE joins) → `[demo] mesh<-...: ... (relaying)` / `[demo] udp ack: ... -- relaying back down mesh` (or `-- self-originated, not echoing back` for the root's own BLE sightings).
- **NODE**: `[demo] mesh up (pconn=1 layer=1)` → `[demo] blebridge started` → periodic heartbeats (`rc=0x0` = success, `rc=0x400b` = not currently connected) and, when a heartbeat's ACK comes back through the mesh, `[demo] ACK #N from ...`.
- If the mesh link ever drops, watch for `[blebridge] scan+adv paused (yielding radio to WiFi)` followed by a successful reconnect and `[blebridge] scan+adv resumed` - that's bug #3's fix working, not a failure.

Not modeled in this test but worth knowing: the mesh router SSID/password are a
Kconfig **default** in the shared `espmesh` component, so treat that default as
a lab/test-network convenience, not a place for production credentials.
