# ADR-004: Node Firmware OS — Zephyr RTOS over Bare Metal / FreeRTOS / Arduino

**Status:** Accepted  
**Date:** 2026-02-25  
**Author:** jknoxdev

---

## Context

The LIMA node firmware must manage concurrent tasks including:
- Continuous sensor polling at different rates (IMU @ 100ms, barometric @ 500ms)
- Hardware-accelerated cryptographic signing on event detection
- BLE stack management and advertisement scheduling
- Power state management (light sleep / deep sleep transitions)
- Watchdog and fault recovery

The firmware OS choice determines scheduling determinism, power management capability, BLE stack quality, and long-term maintainability.

---

## Decision

**Selected: Zephyr RTOS with Nordic Connect SDK (NCS)**

Zephyr RTOS via the Nordic Connect SDK is the firmware operating system for LIMA v1.0.

---

## Alternatives Considered

### Bare Metal (no RTOS)
- ✅ Maximum control, zero overhead
- ✅ Deterministic execution — no scheduler jitter
- ❌ Concurrent sensor polling requires manual interrupt management — complex and error-prone
- ❌ BLE stack integration without RTOS is extremely difficult — Nordic's SoftDevice requires OS abstractions
- ❌ Power management state machine must be hand-coded
- ❌ No HAL — direct register manipulation for every peripheral
- ❌ Not maintainable at LIMA's complexity level

### FreeRTOS
- ✅ Mature, widely deployed, large community
- ✅ Lightweight — runs on very constrained hardware
- ✅ Well documented, AWS IoT support
- ❌ Nordic BLE SoftDevice integration requires significant glue code
- ❌ No native west / NCS toolchain support — separate build system
- ❌ Power management APIs less mature than Zephyr on nRF52840
- ❌ No built-in device tree — peripheral configuration is manual

### Arduino (framework)
- ✅ Extremely accessible, fast prototyping
- ✅ Large library ecosystem
- ❌ Not a real RTOS — no preemptive scheduling, no priority management
- ❌ BLE libraries for nRF52840 are community-maintained, not production-grade
- ❌ No hardware crypto integration with CryptoCell-310
- ❌ Power management is rudimentary
- ❌ Not appropriate for security-critical firmware

### Mbed OS (ARM)
- ✅ ARM-native, good Cortex-M support
- ✅ Real RTOS with proper scheduling
- ❌ Nordic NCS ecosystem not compatible — loses CryptoCell-310 integration
- ❌ Declining community — ARM deprioritizing Mbed
- ❌ BLE stack quality inferior to Nordic's own stack

---

## Rationale

Zephyr + NCS is the only stack that provides all required capabilities as first-class features:

| Requirement                | Zephyr/NCS | Bare Metal | FreeRTOS | Arduino |
| ----------------------------| ------------| ------------| ----------| ---------|
| CryptoCell-310 integration | ✅ native   | ❌ manual   | ⚠️        | ❌       |
| BLE 5.0 Coded PHY          | ✅ native   | ❌          | ⚠️ glue   | ⚠️       |
| Preemptive scheduling      | ✅          | ❌          | ✅        | ❌       |
| Power management APIs      | ✅          | ❌ manual   | ⚠️        | ❌       |
| Device tree config         | ✅          | ❌          | ❌        | ❌       |
| West build system          | ✅          | ❌          | ❌        | ❌       |
| Linux Foundation backed    | ✅          | N/A        | ❌        | ❌       |

**NCS specifically** — Nordic Connect SDK wraps Zephyr with production-tested nRF52840 board support, CryptoCell-310 mbedTLS integration, and a tested BLE stack. This is what Nordic ships in commercial products — the quality bar is production-grade.

**Device tree** is a significant advantage — peripheral configuration (I2C bus, GPIO pins, power domains) is declared in `.dts` files rather than hardcoded, making the firmware portable across hardware revisions without source changes.

**West toolchain** enables reproducible builds via `west.yml` manifest — any developer can clone the repo and reproduce the exact same binary with `west init && west update && west build`.

---

## Consequences

- **Good:** Production-grade BLE stack, CryptoCell-310 integration, deterministic scheduling
- **Good:** West manifest ensures reproducible builds — critical for security firmware
- **Good:** Linux Foundation backing — long-term project viability
- **Good:** Device tree portability — easy to adapt to nRF52833 or nRF5340 in future
- **Bad:** Steep learning curve — Zephyr concepts (device tree, Kconfig, west) take time to internalize
- **Bad:** NCS version lock — west.yml pins exact NCS revision, updates require testing
- **Bad:** Build times longer than Arduino/bare metal due to full RTOS compilation
- **Neutral:** Requires UF2 or J-Link for flashing — no Arduino-style one-click upload
