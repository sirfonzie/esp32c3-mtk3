# Getting Started - µT-Kernel 3.0 on ESP32-C3

This guide walks through installing dependencies, building, flashing, and running the port for the first time. It assumes a Linux or WSL2 host and an ESP32-C3 board connected over USB.

---

## 1. Prerequisites

**Hardware** - see [Hardware in the README](../README.md#hardware) for the
supported boards and links to reference units. You need a data USB cable (not
charge-only) and 4 MB flash, standard on all DevKit boards.

The two board kits differ in how the console is routed, which is the single
thing most likely to trip you up later: the Super Mini/native-USB path uses the
ESP32-C3 built-in USB-Serial-JTAG controller as both flash/monitor port and
kernel console, while M5StampC3 uses its on-board CH9102 USB-UART bridge wired
to ESP32-C3 UART0.

**Software**

- ESP-IDF v5.x or v6.x installed and working (`idf.py --version` should succeed)
- Python 3.8+, CMake 3.16+, Ninja - all installed automatically by the IDF installer
- Git

If you do not have ESP-IDF yet, follow the official guide:
https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/get-started/

---

## 2. Clone the repo

```sh
git clone https://github.com/sirfonzie/esp32c3-mtk3.git
cd esp32c3-mtk3
```

The µT-Kernel 3.0 source is vendored directly into this repository (`kernel/`, `include/`, `lib/`, `device/`, `config/`), so a plain clone gets everything. There are no submodules to initialise.

---

## 3. Set up the ESP-IDF environment

Each terminal session needs the IDF environment loaded:

```sh
. ~/esp/esp-idf/export.sh
```

Then set the target once per checkout:

```sh
idf.py set-target esp32c3
```

---

## 4. Build and flash

### Kernel only (recommended first build)

Use separate build and sdkconfig paths per board. ESP-IDF materializes
`sdkconfig`; changing `SDKCONFIG_DEFAULTS` later does not overwrite an existing
one.

**ESP32-C3 Super Mini / native USB-Serial-JTAG**

```sh
idf.py -B build_supermini \
  -DMTK_BOARD=iote_esp32c3_mini \
  -DSDKCONFIG=build_supermini/sdkconfig \
  -DSDKCONFIG_DEFAULTS=sdkconfig.defaults \
  -p /dev/ttyACM0 build flash monitor
```

**M5StampC3 / CH9102 UART0**

```sh
idf.py -B build_m5stamp_c3 \
  -DMTK_BOARD=iote_m5stamp_c3 \
  -DSDKCONFIG=build_m5stamp_c3/sdkconfig \
  -DSDKCONFIG_DEFAULTS=sdkconfig.defaults.m5stamp_c3 \
  -p /dev/ttyACM0 build flash monitor
```

On Windows the port is usually `COM3` or similar. On macOS it is `/dev/cu.usbmodem*`.

Expected output after boot:

```
[mtkernel] __wrap_esp_startup_start_app: bypassing FreeRTOS
[mtkernel] heap 0x3fc... (32768 bytes)
[mtkernel] calling knl_main()

microT-Kernel Version 3.00

[mtkernel] usermain on micro T-Kernel / ESP32-C3 (preemptive)
[ktest] 8 passed, 0 failed
[ptmr] measured rate: 9994 Hz (expect ~10000)
[adc] ch0=... ch1=... (12-bit raw)
[prod-N] #1  @ 7509 ms  drift=+1 ms
[prod-P] #1  @ 8510 ms  drift=+1 ms
```

The first line - `bypassing FreeRTOS` - confirms µT-Kernel took over from FreeRTOS. The rest of the output is the default demo running.

Press `Ctrl+]` to exit the monitor.

---

## 5. Feature sets

µT-Kernel can be built with optional WiFi and BLE stacks. The feature set is
selected by passing an sdkconfig overlay at build time -
`sdkconfig.defaults.wifi`, `.ble`, or `.wifi_ble`, with no overlay meaning
kernel-only. See
[Feature sets in the README](../README.md#feature-sets) for the overlay table
and what each one pulls in.

The WiFi+BLE combination is marked EXPERIMENTAL - it works but may produce occasional resets under heavy simultaneous radio load.

### Switching feature sets

IDF caches the active configuration in `sdkconfig`. When switching to a different feature set you must clear it:

```sh
idf.py fullclean && rm -f sdkconfig
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults.ble" build   # or .wifi / .wifi_ble / (none)
```

Rebuilding within the same feature set does not need `rm -f sdkconfig` - `idf.py fullclean` is enough.

### What to look for

**WiFi build** - after boot, the kernel tests run and then:
```
[wifi] initialising...
[wifi] AP ready  SSID=MTK3-AP
```
An access point named `MTK3-AP` becomes visible from any WiFi scanner.

**BLE build** - after boot:
```
[ble] NimBLE host task started
[ble] advertising as "MTK3-C3"
```
The device `MTK3-C3` appears in nRF Connect or any BLE scanner.

---

## 6. Building examples

Each example under `examples/` is a standalone IDF project. Build and flash without leaving the repo root:

```sh
idf.py -C examples/four_tasks build
idf.py -C examples/four_tasks -p /dev/ttyACM0 flash monitor
```

Examples that use WiFi or BLE already have their own `sdkconfig.defaults` for
the default Super Mini/native USB path. For M5StampC3, pass
`-DMTK_BOARD=iote_m5stamp_c3` and layer the M5 UART0 defaults after the
example's own defaults:

```sh
idf.py -C examples/four_tasks -B build_four_tasks_m5stamp_c3 \
  -DMTK_BOARD=iote_m5stamp_c3 \
  -DSDKCONFIG=../../build_four_tasks_m5stamp_c3/sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;../../sdkconfig.defaults.m5stamp_c3" \
  -p /dev/ttyACM0 build flash monitor
```

There are 12 examples. Rather than repeat them here, see
[`../examples/README.md`](../examples/README.md) - it lists what each one
demonstrates, the exact build and flash command for both board kits, the
hardware each needs (some want two C3 boards, an I²C device, or a router), and
the config settings that differ from the defaults.

Good first examples: `tasks` (smallest real µT-Kernel app), then `four_tasks`
(priority-preemptive scheduling with live timestamps). The kernel's own API
smoke tests run automatically in the root demo app - see §4.

---

## 7. Troubleshooting

### Board not detected (`/dev/ttyACM0` missing)

- Check `lsusb` - the C3's USB-JTAG shows as `Espressif USB JTAG/serial debug unit`
- On WSL2: attach the device from Windows first using `usbipd attach --wsl --busid <id>`
- Try a different USB cable - charge-only cables have no data lines

### Flash fails with timeout

The ESP32-C3 must be in download mode. Hold the BOOT button while pressing RESET, then release RESET first, then BOOT. Most DevKit boards do this automatically, but not all.

### Monitor shows garbage characters

For Super Mini/native USB-Serial-JTAG, the console does not use a fixed UART
baud rate. If monitor output is wrong, confirm the IDF target is `esp32c3` and
that you built with `sdkconfig.defaults`.

For M5StampC3, the CH9102 bridge uses UART0 at 115200 baud. Build with
`sdkconfig.defaults.m5stamp_c3`; a Super Mini USB-JTAG `sdkconfig` will not work
on the M5 console.

### WSL2: flash hangs or errors with pyserial

The ESP32-C3's USB-Serial-JTAG port does not respond correctly to `select()` write checks on WSL2. The `esptool.cfg` in the repo root sets `serial_write_timeout = 0` to work around this. If flashing still fails:

1. Confirm `esptool.cfg` is present in the directory you are running `idf.py` from
2. Detach and re-attach the USB device from Windows: `usbipd detach --busid <id>` then `usbipd attach --wsl --busid <id>`

### BLE: build error - `STAILQ_ENTRY` / `SLIST_ENTRY` undeclared in NimBLE headers

µT-Kernel ships its own `include/sys/queue.h`, which shadows the toolchain's
BSD `sys/queue.h` whenever the kernel include directory is on the `-I` list.
NimBLE's `os/queue.h` then finds the wrong header and `STAILQ_ENTRY` is
undefined.

**Fix:** any file that includes NimBLE host headers must live in a component
that does **not** list `mtkernel` in its `REQUIRES` - `components/ble_stack/`
and `components/net/blebridge/` are both built that way. Forward-declare the
kernel calls you need with their raw ABI types instead of including
`<tk/tkernel.h>`:

```c
extern int  tk_dly_tsk(unsigned long dlytim);  /* ER = int, RELTIM = unsigned long */
extern void tk_ext_tsk(void);
```

### BLE: build error - `MSTATUS_MIE` / `MSTATUS_MPIE` / `MSTATUS_MPP` redefined

`<tk/tkernel.h>` and `esp_bt.h` both define these RISC-V CSR bit macros. Never
include both in the same translation unit; split the kernel-facing and
BLE-facing code into separate files.

### BLE: `nimble_port_init` returns `ESP_ERR_INVALID_STATE` (259)

You called `esp_bt_controller_init()` and `esp_bt_controller_enable()` before
`nimble_port_init()`, which unconditionally re-initialises them.

**Fix:** remove the separate controller calls and let `nimble_port_init()` own
the whole sequence.

### BLE: panic with a load access fault at `MTVAL=0x10` after `phy_init`

`coex_schm_status_bit_clear()` dereferences a coexistence context pointer that
is NULL when BLE starts without a prior WiFi init.

**Fix:** for BLE-only builds leave `CONFIG_ESP_COEX_SW_COEXIST_ENABLE` off and
use hardware CCA. For WiFi+BLE builds enable software coexistence *and*
initialise WiFi (or the mesh) before BLE.

### WiFi + BLE: the mesh child never finishes joining

The mesh child does not scan or switch channels. Its configured channel must
exactly match the network's actual channel, or WiFi will associate while the
mesh handshake stalls. Set `CONFIG_ESPMESH_CHANNEL` to your router's fixed
channel via `idf.py menuconfig` -> "MTK3 ESP-MESH".

### `sdkconfig` has unexpected values after switching feature sets

The `sdkconfig` file caches the last build configuration. Always clear it when
switching feature sets in the same build directory:

```sh
idf.py fullclean && rm -f sdkconfig
```

For switching boards, the less error-prone workflow is separate build
directories: `build_supermini` and `build_m5stamp_c3`.

---

## 8. Next steps

- [`../examples/README.md`](../examples/README.md) - every example, including the BLE+ESP-MESH multi-board test
- [`PORT_ESP32C3.md`](PORT_ESP32C3.md) - feature status: what is validated on hardware, what is ported but unvalidated, and what is not ported at all
- `components/freertos_shim/` - the FreeRTOS API layer the WiFi and BLE stacks run on; read this to see how the radios are grafted onto µT-Kernel
- Read `components/mtkernel/` for the kernel source and BSP
- The µT-Kernel 3.0 specification is at https://tron-forum.github.io/mtkernel_3/
