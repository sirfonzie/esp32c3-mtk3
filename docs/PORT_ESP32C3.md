# µT-Kernel 3.0 on ESP32-C3 - feature status

First port of µT-Kernel 3.0 to a RISC-V CPU and to the Espressif ESP-IDF build
system. Targets **ESP32-C3 Super Mini / native USB-Serial-JTAG** and
**M5StampC3 / CH9102 UART0** (single-core RV32IMC, 160 MHz).

This document states what works, what is implemented but not proven, and what
is absent. Each entry is one of three states:

- **Validated** - implemented and confirmed on real hardware, with the result recorded below.
- **Ported, not validated** - implemented and believed working, but without a rigorous or long-running hardware test behind it. Use with that in mind.
- **Not ported** - absent from this port. Calling it will fail, or the feature simply does not exist.

Validation dates and the board used are given where they are known. The test
harnesses that produced these results are not part of this release; see
[Performance numbers](../README.md#performance-numbers) in the README.

---

## Validated

### Kernel core

| Feature | Notes |
|---|---|
| Preemptive multitasking | Timer-driven preemption across tasks of equal or higher priority. Two same-priority tasks with phase-shifted 1 s delays alternate at a 500 ms cadence. |
| Task management | Create, start, delay, terminate, suspend/resume, change priority. Heap delta 0 across create/delete cycles. |
| Priority scheduling | Higher-priority task runs first; immediate preemption on `tk_sta_tsk`. |
| Semaphore | Round-trip 31 ms, heap delta 0. |
| Mutex | No critical-section violation across 10 lock/unlock cycles. |
| Priority inheritance | LOW elevated to priority 3 while MED blocked and HIGH held the mutex. |
| Event flag | Bit-0 wait completes in 21 ms. |
| Mailbox | Payload received intact. |
| Message buffer | Payload received intact; sender blocks when full. |
| Fixed-size memory pool | 8 allocations, exhaustion correctly detected, data intact. |
| Variable-size memory pool | 4 allocations, heap delta 0. |
| Cyclic and alarm handlers | Both fire on schedule; covered by the boot-time smoke tests. |
| Idle task | 5/5 ticks in 201 ms; `wfi` parks the CPU when nothing is ready. |
| Stack canary | All canaries intact; detection mechanism itself verified. |
| ISR context tracking | `knl_taskindp` correctly tracks task-independent context; ISR fired 3/3. |
| Register preservation | 15 registers verified across ~180 k forced context switches, 257 M clean iterations per task, 0 errors. |
| Optimization-level robustness | Identical results at -O0, -Og, -O2 and -Os (49/49 at every level). |
| Long-run stability | 2 h soak: 120/120 snapshots, heap flat at 277,644 bytes (net leak 0), tick error 1 ms throughout, 0 scheduling anomalies, 0 watchdog resets, 711,690 semaphore cycles. |

### Time and interrupts

| Feature | Notes |
|---|---|
| 1 ms kernel tick | `CNF_TIMER_PERIOD` = 1 ms, backed by SYSTIMER. Tick error stayed at 1 ms across the 2 h soak. |
| `tk_def_int` user IRQ registration | Registers a handler and fires it; activated with `EnableInt`. |
| CPU interrupt levels | `SetCpuIntLevel`/`GetCpuIntLevel` operate the ESP32-C3 INTC threshold. Masking behaviour verified for LEVEL1/2/3 sources at every level. Levels are per-task state and survive preemption. |
| `E_CTX` enforcement | Blocking calls at any level above `INTLEVEL_EI` correctly return `E_CTX`. |
| Latched wakeups | Non-blocking calls issued under `DI()` or at a raised level pend the dispatch interrupt; preemption fires the moment the level drops, not a tick later. |
| Tick catch-up | Missed periods are replayed after a stall, capped at one second's worth. A 1.5 s stall at level 3 credits 1.0 s and slips 0.5 s, as designed. |
| Interrupt watchdog | INT-WDT armed by the port and fed from the kernel tick. A CPU wedge recovers via hardware reset, reporting `reset_reason=5` on the next boot. |
| Panic diagnostic | A forced store fault prints the faulting µT-Kernel task before IDF's Guru Meditation dump. |
| Interrupt and dispatch latency | Trigger to handler 1.09 µs (max 4.7 µs); semaphore-signal-in-ISR to woken task 2.52 µs (max 31.5 µs under tick collisions). Tick jitter no more than 2 µs when idle and under CPU load. |

### Device drivers

| Feature | Notes |
|---|---|
| Physical timers (`tk_*ptmr`) | TIMG0/T0 and TIMG1/T0, 1 MHz (1 µs resolution), one-shot and periodic. Measured 9,994 Hz against a nominal 10 kHz. |
| `DEV_SER` serial driver | UART1, exposed as `/sera`, with internal loopback verified. Super Mini board kit only - see [Ported, not validated](#ported-not-validated). |
| `DEV_ADC` | ADC1 channels 0-4 (GPIO0-GPIO4), 12-bit raw, polling one-shot conversions. |
| T-Monitor console | TX and RX confirmed on both board kits - USB-Serial-JTAG on Super Mini, UART0/CH9102 on M5StampC3. |
| Board kits | Both Super Mini and M5StampC3 built, flashed and confirmed on real hardware. |

### Radio

| Feature | Notes |
|---|---|
| BLE advertising | 12 h advertising soak with the interrupt watchdog armed: zero resets, zero panics. |
| BLE GATT server | Connection, characteristic read, notification subscribe and write-echo all confirmed with a phone scanner. |
| WiFi / lwIP | UDP exchange between two independent boards: 340 rounds over ~11 minutes, 338/340 delivered (99.4%), RTT min 4 / avg 13 / max 49 ms. Both losses coincided with one board being physically moved, so they are RF, not stack. |

---

## Ported, not validated

Implemented and working in normal use, but without long-running or rigorous
hardware validation behind them.

| Feature | What is missing |
|---|---|
| WiFi + BLE concurrent operation | Marked **EXPERIMENTAL**. Both radios run together with hardware coexistence, but occasional resets have been seen under heavy simultaneous load. Not characterised. |
| ESP-MESH | Root and child join, relay messages and coexist with BLE, but only bring-up testing has been done. The mesh child requires its channel to exactly match the network channel - it does not scan or switch. |
| ESP-NOW | Board-to-board exchange works; no sustained-load or long-run testing. |
| Radio stability over time | WiFi and BLE have been soak-tested for **1-2 hours**, not days. Behaviour under heap pressure, repeated connect/disconnect cycles, or high sustained packet rates is uncharacterised. Treat the radios as functional but not production-hardened. |
| 24 h kernel-time drift under flash load | The intended 24 h run was stopped after 14 minutes. What was measured was clean - drift 0 ms, 0 slips, 168 flash writes - but the full duration was never completed. |
| `DEV_SER` concurrent access | Single-task send/receive only. The upstream mSDI layer holds the per-device mutex across the read syscall, so concurrent TX and RX from two tasks on the same device handle will deadlock. Use a send-then-poll pattern from one task. |
| `DEV_SER` on M5StampC3 | Not registered on that board. The driver maps UART1 onto GPIO21/GPIO20, which M5StampC3 needs for the UART0/CH9102 console. |

---

## Not ported

| Feature | Status |
|---|---|
| I2C driver | Not implemented. The `device/i2c/sysdepend/esp32c3/` layer does not exist; deferred because it needs an external I2C device to demonstrate. IDF's own I2C driver remains available to applications. |
| Task exception containment | A task fault reboots the system. The panic diagnostic identifies which task faulted, but synchronous exceptions are not mapped to µT-Kernel task termination. |
| Debug hooks (`td_hok_svc`, `td_hok_dsp`, `td_hok_int`) | Declared in `dbgspt.h` with no implementation in the kernel this port builds on. Needed for timestamp-accurate task-switch and ISR profiling. |
| Task exception API (`tk_def_tex`, `tk_ras_tex`) | Not present in this kernel configuration. |
| `tk_get_cfn` | Not present in this kernel configuration. |
| Rendezvous ports (`tk_cre_por`, `tk_cal_por`, `tk_acp_por`, `tk_rpl_rdv`) | A legacy API, compiled out unless the kernel is built with `USE_LEGACY_API=1`. Enabling that flag currently panics at boot on this port - a latent issue in the legacy init path, separate from the active API surface. |

---

## Non-goals

These are deliberate constraints, not gaps.

- **No FPU.** The ESP32-C3 has no floating-point unit, so `USE_FPU=0` is correct.
- **Single-core only.** This port assumes the single-core ESP32-C3 throughout; it makes no attempt at SMP.

---

## Validation history

The results above come from three recorded hardware runs:

| Date | Board | Scope |
|---|---|---|
| 2026-06-12 | ESP32-C3 Super Mini | 15 functional tests (15/15 PASS) followed by a 2 h soak (PASS). 3 port gaps recorded, all debug hooks, none a test failure. |
| 2026-07-06 | ESP32-C3 Super Mini | BLE advertising soak, optimization-level matrix, register storm, flash-write drift (partial), latency histograms, and a two-board WiFi/UDP exchange. |
| 2026-07-16 | Both board kits | Both boards built, flashed and confirmed; M5StampC3 console path settled on UART0/CH9102 ownership of GPIO21/GPIO20. |


Testing was done against **ESP-IDF v5.x and v6.x**, primarily v6.1-dev.
Embedded behaviour depends heavily on hardware revision, IDF version and usage
pattern, so if you hit something these runs did not cover, please open an issue
with the `idf.py monitor` output, your IDF version, and your board revision.
