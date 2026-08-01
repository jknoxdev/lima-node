# ADR-001: Edge Node MCU — nRF52840 over ESP32 / STM32

**Status:** Accepted  
**Date:** 2026-02-25  
**Author:** jknoxdev

---

## Context

LIMA requires an edge node MCU capable of:
- Low-power continuous sensor polling (IMU + barometric)
- Hardware-accelerated cryptographic signing of events
- Bluetooth Low Energy 5.0 with extended range (Coded PHY)
- Running a real-time operating system for deterministic sensor response
- Operating on battery for extended periods in field deployments

The MCU selection is the most foundational decision in the system — it determines the entire firmware stack, toolchain, power budget, and cryptographic capabilities.

---

## Decision

**Selected: Nordic Semiconductor nRF52840 (Cortex-M4F @ 64MHz)**

The nRF52840 is the primary edge node MCU for LIMA v1.0.

---

## Alternatives Considered

### ESP32 (Espressif)
- ✅ Cheap, widely available, large community
- ✅ WiFi + BLE dual radio
- ❌ No hardware cryptographic accelerator — signing in software is slow and power-hungry
- ❌ WiFi radio dominates power budget — unsuitable for battery-powered deployments
- ❌ BLE 4.2 only on most variants — no Coded PHY for extended range
- ❌ Xtensa LX6 architecture — less mature RTOS support vs ARM Cortex-M

### STM32 (STMicroelectronics)
- ✅ Excellent Cortex-M ecosystem, mature HAL
- ✅ Low power modes well documented
- ❌ No integrated BLE radio — requires external module, adds BOM cost and complexity
- ❌ No hardware crypto accelerator on most low-power variants
- ❌ External BLE module adds failure points and I2C/UART integration complexity

### nRF9160 (Nordic — LTE-M/NB-IoT)
- ✅ Cellular connectivity — no local gateway required
- ❌ Cellular subscription cost per node — unacceptable for air-gapped deployments
- ❌ Cellular introduces external network dependency — violates local-first design principle
- ❌ Higher power consumption than BLE

---

## Rationale

The nRF52840 uniquely satisfies all requirements simultaneously:

| Requirement          | nRF52840         | ESP32 | STM32 |
| ----------------------| ------------------| -------| -------|
| HW crypto (ECDSA)    | ✅ CryptoCell-310 | ❌     | ❌     |
| BLE 5.0 Coded PHY    | ✅                | ❌     | ❌     |
| Zephyr RTOS support  | ✅ First-class    | ⚠️     | ✅     |
| Deep sleep < 2µA     | ✅                | ❌     | ✅     |
| Integrated radio     | ✅                | ✅     | ❌     |
| NCS / West toolchain | ✅                | ❌     | ❌     |

The **CryptoCell-310** hardware accelerator is the decisive factor — it enables ECDSA-P256 signing in ~50ms at minimal power cost. Software signing on ESP32 takes 10-20x longer and drains battery significantly faster.

**BLE 5.0 Coded PHY** doubles the effective range over BLE 4.2 at the same transmit power — critical for deployments across large enclosures or vehicle fleets.

---

## Consequences

- **Good:** Hardware crypto, extended BLE range, deep sleep power profile, first-class Zephyr/NCS support
- **Good:** Makerdiary MDK USB Dongle form factor — compact, USB-programmable, production-ready
- **Bad:** Higher unit cost vs ESP32 (~$10 vs ~$4)
- **Bad:** Nordic ecosystem lock-in — NCS/West toolchain is Nordic-specific
- **Neutral:** Requires UF2 bootloader workflow for programming (documented in `/docs/`)
