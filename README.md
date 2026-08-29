# µT-Kernel 3.0 - ESP32-C3 Port

µT-Kernel 3.0 (IEEE 2050-2018) running natively on the **ESP32-C3** (RISC-V, single-core) via ESP-IDF. The port replaces the FreeRTOS scheduler at startup and runs µT-Kernel as the sole RTOS scheduler. ESP-IDF hardware drivers and WiFi/BLE stacks remain available through the port integration and the necessary FreeRTOS API shim.

**Current capabilities**

- **Full preemptive kernel** - tasks, priorities, semaphores, event flags, mutexes with priority inheritance, mailboxes, message buffers, fixed and variable memory pools, cyclic and alarm handlers, and physical timers, all verified on hardware
- **ESP-IDF peripherals and radios retained** - device drivers for UART and ADC, plus WiFi, lwIP, ESP-NOW, ESP-MESH and BLE (NimBLE), reached through a FreeRTOS API shim that runs them on µT-Kernel primitives
- **Two board kits, four build configurations** - ESP32-C3 Super Mini (native USB-JTAG) and M5StampC3 (CH9102 UART0), each buildable as kernel-only, +WiFi, +BLE, or +WiFi+BLE
- **12 standalone examples** to try, from a two-task template to a two-board BLE + ESP-MESH bridge

See [docs/PORT_ESP32C3.md](docs/PORT_ESP32C3.md) for exactly what is validated on hardware, what is ported but not yet validated, and what is not ported.

> [!NOTE]
> This project is developed and maintained independently without funding or commercial support. If you intend to try, test, or follow this port, please consider giving the repository a star. It helps demonstrate interest in the project and supports its continued development.

---

## Hardware

- ESP32-C3 Super Mini or another ESP32-C3 board with native USB-Serial-JTAG ([reference hardware](https://www.aliexpress.com/item/1005007170832747.html))
- M5StampC3 with on-board CH9102 USB-UART bridge ([reference hardware](https://www.aliexpress.com/item/1005003469469934.html))
- USB cable (data, not charge-only)
- 4 MB flash

---

## Quick start

```sh
git clone https://github.com/sirfonzie/esp32c3-mtk3.git
cd esp32c3-mtk3

# set up ESP-IDF (v5.x or v6.x - replace with your actual IDF path)
. ~/esp/esp-idf/export.sh
idf.py set-target esp32c3

# Super Mini / native USB-Serial-JTAG, kernel only -- no radio
idf.py -B build_supermini \
  -DMTK_BOARD=iote_esp32c3_mini \
  -DSDKCONFIG=build_supermini/sdkconfig \
  -DSDKCONFIG_DEFAULTS=sdkconfig.defaults \
  -p /dev/ttyACM0 build flash monitor

# M5StampC3 / CH9102 UART0 console
idf.py -B build_m5stamp_c3 \
  -DMTK_BOARD=iote_m5stamp_c3 \
  -DSDKCONFIG=build_m5stamp_c3/sdkconfig \
  -DSDKCONFIG_DEFAULTS=sdkconfig.defaults.m5stamp_c3 \
  -p /dev/ttyACM0 build flash monitor
```

See [docs/GETTING_STARTED.md](docs/GETTING_STARTED.md) for a full step-by-step walkthrough.

---

## Documentation

| Doc | Covers |
|---|---|
| [docs/GETTING_STARTED.md](docs/GETTING_STARTED.md) | Step-by-step install, build, flash, troubleshooting |
| [docs/PORT_ESP32C3.md](docs/PORT_ESP32C3.md) | Feature status: what is validated on hardware, what is ported but not validated, and what is not ported at all |
| [examples/README.md](examples/README.md) | Every standalone example: what it demonstrates, how to build/flash it, and any config that differs from the defaults |

---

## Feature sets

The build is controlled by a single `-DSDKCONFIG_DEFAULTS` flag. Each configuration is self-contained - no manual editing of `sdkconfig` is needed.

| Feature set | Build command | Includes |
|---|---|---|
| Kernel only | `idf.py build` | Scheduler, IPC, timers, device drivers |
| + WiFi | `idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults.wifi" build` | + esp_wifi, lwIP, esp_netif, ESP-MESH |
| + BLE | `idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults.ble" build` | + NimBLE host stack |
| + WiFi + BLE | `idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults.wifi_ble" build` | Both radios with HW coexistence **[EXPERIMENTAL]** |

To switch between configurations:

```sh
idf.py fullclean && rm -f sdkconfig
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults.ble" build   # or .wifi / .wifi_ble / (none for kernel-only)
```

For board switching, prefer separate `-B` and `-DSDKCONFIG` paths as shown in
Quick start. The Super Mini uses `sdkconfig.defaults` and the native
USB-Serial-JTAG console; M5StampC3 uses `sdkconfig.defaults.m5stamp_c3` and
UART0 through its CH9102 bridge. Defaults do not overwrite an already-created
project `sdkconfig`.

---

## Default demo (what you see on first flash)

```
[mtkernel] __wrap_esp_startup_start_app: bypassing FreeRTOS
[ktest] 8 passed, 0 failed
[smoke] handler fired 2 times (expect 2)
[ptmr] measured rate: 9994 Hz (expect ~10000)
[adc] ch0=2218 ch1=2312 ch2=4095 ch3=2408 ch4=2270 (12-bit raw)
[tm] press any key within 5s for interactive RX test...
[prod-N] #1  @ 7509 ms  exp=7508  drift=+1 ms     <- naive scheduling
[prod-P] #1  @ 8510 ms  exp=8509  drift=+1 ms     <- phase-locked (stays +-1 ms forever)
```

The multi-task section runs two producer tasks: one using naive `tk_dly_tsk` (drift grows ~1 ms/cycle) and one using phase-locked absolute deadline scheduling (drift stays ±1 ms indefinitely). Both send to a shared mailbox consumer.

---

## Examples

Each example is a standalone IDF project under `examples/`. For the default
Super Mini/native USB-Serial-JTAG board kit:

```sh
idf.py -C examples/<name> build
idf.py -C examples/<name> -p /dev/ttyACM0 flash monitor
```

For M5StampC3, keep the example's own defaults and layer the board console
defaults after them:

```sh
idf.py -C examples/<name> -B build_<name>_m5stamp_c3 \
  -DMTK_BOARD=iote_m5stamp_c3 \
  -DSDKCONFIG=build_<name>_m5stamp_c3/sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;../../sdkconfig.defaults.m5stamp_c3" \
  -p /dev/ttyACM0 build flash monitor
```

Examples that use WiFi or BLE have their own `sdkconfig.defaults` already
configured. See [examples/README.md](examples/README.md) for exact commands and
hardware notes.

| Example | Requires | Description |
|---|---|---|
| `four_tasks` | Kernel only | Four tasks at different priorities and periods |
| `tasks` | Kernel only | Minimal two-task preemption template |
| `adc` | Kernel only | ADC1 channels 0-4 (GPIO0-GPIO4) |
| `i2c_scan` | Kernel only | Walks 7-bit I2C address space (SDA=GPIO6, SCL=GPIO7) |
| `pwm` | Kernel only | LEDC PWM on GPIO10, stepping duty 0-100% |
| `uart_loopback` | Kernel only | UART1 TX/RX via the DEV_SER device driver (needs a GPIO21-GPIO20 jumper to see the RX side) |
| `pingpong_c3` | **WiFi** | ESP-NOW C3-to-C3 ping-pong; flash same binary on two C3 boards |
| `udp_pingpong_c3` | **WiFi** | lwIP UDP C3-to-C3 ping-pong; flash same binary on two C3 boards (needs a router; set `WIFI_SSID`/`WIFI_PASSWORD` in source) |
| `ble_beacon` | **BLE** | Non-connectable broadcaster; verify with nRF Connect Scanner - appears as `MTK3-Beacon` with manufacturer data |
| `ble_gatt` | **BLE** | Connectable GATT server `MTK3-GATT`; nRF Connect can read heap/uptime, subscribe for 3 s notifications, and write an echo characteristic |
| `ble_espmesh_demo` | **WiFi + BLE** | ESP-MESH child/node plus BLE scan/advertise bridge |
| `ble_espmesh_root` | **WiFi + BLE** | ESP-MESH root plus BLE scan/advertise and UDP relay; optional external UDP responder listens on port 5567 |

---

## Performance numbers

Comparative performance results for this port - microbenchmark suites,
throughput measurements, and cross-kernel comparisons against FreeRTOS on the
same silicon - are being prepared for academic publication. **The benchmark
harnesses and the numbers they produce are therefore not part of this release.**
Both will be published here once the paper is out, so that the results are
reproducible from this repository.

The test harnesses that produced them - interrupt/dispatch/tick-jitter
histograms, kernel-time drift under flash load, register-preservation storms,
and the functional and conformance suites - are held back with the numbers, and
will be published alongside them.

The hardware validation results already obtained from those harnesses are
recorded in [docs/PORT_ESP32C3.md](docs/PORT_ESP32C3.md), with the date and
board of each run. The examples that ship here demonstrate the kernel and its
drivers; they are not a validation suite.

---

## Project structure

```
components/
  mtkernel/         µT-Kernel 3.0 core + ESP32-C3 BSP (RISC-V port, device drivers)
  freertos/         Shadow of IDF's freertos component, scheduler sources excluded
  freertos_shim/    FreeRTOS API surface backed by µT-Kernel primitives
  ble_stack/        WiFi init + NimBLE init (root demo app only)
  net/espmesh/      ESP-MESH wrapper      (opted into explicitly by an example)
  net/blebridge/    BLE scan/advertise bridge over an ESP-MESH uplink
main/
  main.c            Default demo entry point (usermain)
  smoke_kernel_api.c
  app_multitask.c   Naive vs phase-locked producer/consumer demo
examples/           Standalone IDF apps (one per feature)
LICENSES/           Full licence texts - see Licensing at the bottom of this file
sdkconfig.defaults  Kernel-only base settings (auto-loaded when no overlay given)
sdkconfig.defaults.m5stamp_c3
                    Kernel-only M5StampC3 settings (UART0 console via CH9102)
sdkconfig.defaults.wifi
                    WiFi feature overlay
sdkconfig.defaults.ble
                    BLE feature overlay
sdkconfig.defaults.wifi_ble
                    WiFi+BLE feature overlay [EXPERIMENTAL]
```

---

## WSL2 note

The ESP32-C3 USB-Serial-JTAG port does not respond correctly to `select()` write-readiness checks on WSL2. The `esptool.cfg` in the repo root sets `serial_write_timeout = 0`, switching pyserial to non-blocking writes. `idf.py flash` picks this up automatically - no changes to pyserial or esptool are needed.

---

## Testing and Bug Reports

This port has been developed and validated against **ESP-IDF v5.x and v6.x** (primarily tested on v6.1-dev). Testing covered kernel API smoke tests, functional conformance tests, hardware-in-the-loop testing across all four build configurations (kernel-only, WiFi, BLE, WiFi+BLE), and extended multi-task scheduling runs on physical ESP32-C3 hardware. All known issues have been resolved to the best of our ability prior to this release.

The suites that produced those results - the conformance, functional and microbenchmark harnesses - are not part of this release; they are held back with the numbers being prepared for publication (see [Performance numbers](#performance-numbers) above) and will be published alongside them. What ships here is the port and its examples. The one check that runs on every boot is the kernel API smoke suite in the default demo app, which prints `[ktest] 8 passed, 0 failed`.

**Soak testing note:** Radio features (WiFi and BLE) have been validated for basic functionality but have only been soak-tested for **1–2 hours** rather than days. Long-running stability under sustained radio load - including behaviour under heap pressure, repeated connect/disconnect cycles, or high packet rates over extended periods - has not been fully characterised. These features should be considered functional but not production-hardened. The kernel-only configuration has received the most thorough testing.

That said, embedded software is highly dependent on hardware revision, ESP-IDF version, and usage patterns that may not have been covered during testing. If you encounter unexpected behaviour, a crash, or a reproducible failure, please open a GitHub issue - there is a **Bug report** template that asks for the few things needed to chase it down: what happened, how to reproduce it, the `idf.py monitor` output at the point of failure, and your ESP-IDF version and board.

All reports are welcome and will be looked into. Contributions and pull requests are equally appreciated.

**Where help is most useful:** [docs/PORT_ESP32C3.md](docs/PORT_ESP32C3.md) lists
what is validated on hardware, what is ported but not yet validated, and what is
not ported at all. The "Ported, not validated" entries - WiFi+BLE coexistence
under load, ESP-MESH, ESP-NOW, long-run radio stability - are where an extra
pair of hands and a second board would make the most difference. If you run one
of these on your own hardware, there is a **Validation report** issue template
for exactly that. A result is worth having whether it worked or not; "ran for
six hours, no resets" is a real data point.

---

## How the port works

ESP-IDF's startup sequence normally hands control to FreeRTOS after the ROM bootloader and IDF hardware initialisation complete. This port intercepts that handover using the linker's `--wrap` mechanism: `__wrap_esp_startup_start_app` and `__wrap_rtos_int_exit` are substituted for their FreeRTOS equivalents, redirecting control to the µT-Kernel initialisation path (`knl_main()`) instead. From that point, µT-Kernel owns the scheduler, task context switching (via RISC-V software interrupt), IPC primitives (semaphores, event flags, mutexes, mailboxes, memory pools), cyclic and alarm handlers, and the physical timer layer. The FreeRTOS scheduler sources (`tasks.c`, `queue.c`, `timers.c`, `event_groups.c`, `portasm.S`) are excluded from the build; in their place, a thin compatibility shim (`components/freertos_shim/`) translates the FreeRTOS API surface used internally by the ESP-IDF WiFi and BLE stacks into equivalent µT-Kernel calls. The result is a fully preemptive, IEEE 2050-2018 compliant RTOS running on bare ESP32-C3 hardware while retaining access to all ESP-IDF peripheral drivers, WiFi, and BLE.

On ESP32-C3, sysdepend interrupt control now has two layers. `disint()`/`enaint()` save and restore the RISC-V `mstatus.MIE` bit for full CPU critical sections, while `SetCpuIntLevel()`/`GetCpuIntLevel()` operate the ESP interrupt threshold used by `ENABLE_INTERRUPT_UPTO()` and CPU-lock state checks. Task context is restored with the normal enabled threshold (`INTLEVEL_EI`), and full CPU-lock state is represented by `INTLEVEL_DI`.

---

## About µT-Kernel 3.0

µT-Kernel 3.0 is a real-time OS for small-scale embedded systems and IoT edge nodes, developed by TRON Forum.

- Compliant with IEEE Standard 2050-2018; highly compatible with µT-Kernel 2.0
- Source code fully reviewed for portability to modern microprocessors
- Not tied to any particular development environment
- Released as open source under T-License 2.2

Specification: [tron-forum.github.io/mtkernel_3](https://tron-forum.github.io/mtkernel_3/)  
Upstream source: [github.com/tron-forum/mtkernel_3](https://github.com/tron-forum/mtkernel_3)  
TRON Forum: [www.tron.org](https://www.tron.org)

---

## Author

Muhamed Fauzi Bin Abbas, with AI Assistants.

---

## Licensing

This repository combines upstream µT-Kernel 3.0 source with port work written
for the ESP32-C3. Nothing here is relicensed: upstream copyright headers are
left intact in every file, and where this section and a file header disagree,
**the file header governs**. Full licence texts are in [`LICENSES/`](LICENSES/).

| Part of the repository | Copyright | Licence |
|---|---|---|
| `kernel/`, `include/`, `lib/`, `device/`, `config/` | µT-Kernel 3.0, © 2006-2023 Ken Sakamura, released by TRON Forum | T-License 2.2 - [`T-License-2.2_TEF000-219-200401.pdf`](LICENSES/T-License-2.2_TEF000-219-200401.pdf) |
| `components/mtkernel/`, `components/freertos_shim/`, `components/ble_stack/`, `components/net/`, `main/`, `examples/` | ESP32-C3 port work, board support, the FreeRTOS compatibility layer, and the example applications. Derived from and built against ESP-IDF; distributed on the same terms. | Apache-2.0 - [`Apache-2.0.txt`](LICENSES/Apache-2.0.txt) |
| `components/freertos/linker.lf`, `components/freertos/linker_common.lf` | Verbatim copies of ESP-IDF's own linker fragments, kept so the shadow component reproduces IDF's section placement. © Espressif Systems (Shanghai) CO LTD. | Apache-2.0 - [`ESP-IDF_LICENSE.txt`](LICENSES/ESP-IDF_LICENSE.txt) |

### A note on T-License 2.1

TRON Forum releases µT-Kernel 3.0 as a whole under **T-License 2.2**, and the
upstream repository names `TEF000-219-200401.pdf` as the governing document.

50 files still carry "T-License 2.1" in their headers, predating the
relicense - 36 in `kernel/tkernel/`, the rest spread across `include/sys/`,
`kernel/knlinc/`, `kernel/tstdlib/`, `kernel/usermain/`, `lib/libtk/`,
`include/tk/` and `include/tm/`. T-License 2.1 ([`T-License-2.1_TEF000-218-150401.pdf`](LICENSES/T-License-2.1_TEF000-218-150401.pdf))
is included in `LICENSES/` so those headers resolve to a text present in this
repository. This is a record of what the files say, not a claim that any part
of µT-Kernel 3.0 is offered under 2.1 rather than 2.2.

### Build dependencies (not redistributed here)

**ESP-IDF** is required to build this project but is not included in it. It is
Apache-2.0; its own bundled third-party components carry additional licences
documented in that repository. The only ESP-IDF files reproduced here are the
two linker fragments named above.

The **ESP32-C3 WiFi and BLE controller libraries** are distributed by Espressif
as binary blobs inside ESP-IDF, under the Espressif MIT licence. They are not
redistributed here.

### Sources of the licence texts

| Licence | Obtained from |
|---|---|
| T-License 2.2 | `github.com/tron-forum/mtkernel_3` -> `docs/TEF000-219-200401.pdf` |
| T-License 2.1 | `github.com/tron-forum/tkernel_2` -> `TEF000-218-150401.pdf` |
| Apache-2.0 | `apache.org/licenses/LICENSE-2.0.txt` |
| ESP-IDF LICENSE | `github.com/espressif/esp-idf` -> `LICENSE` |

ESP-IDF's `LICENSE` file is byte-identical to the Apache-2.0 text; both are
kept so each attribution above resolves to a file of its own name.
