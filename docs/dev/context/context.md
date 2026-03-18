# LIMA Project — Current State Context

> **Last updated:** 2026-03-09
> **Branch:** `docs`
> **Purpose:** Snapshot for handoff to web Claude or new dev session. Read this first.

---

## What Is This

**L.I.M.A. — Local Integrity Multi-modal Architecture**

A resilient, low-power Physical Intrusion Detection System (PIDS). Edge nodes (nRF52840) detect physical integrity events (door opens, enclosure breaches, vehicle towing, pressure changes), cryptographically sign them with an on-chip hardware accelerator, and broadcast signed packets over BLE to a local gateway. No cloud dependency for core operation.

**Repo:** `github.com/jknoxdev/lima-node`

---

## Hardware

| Component | Part | Role |
|---|---|---|
| Edge Node | Nordic nRF52840 MDK USB Dongle | Sensor + crypto + BLE |
| IMU | MPU6050 | Motion / vibration detection |
| Barometric | BME280 (was BMP280 in early refs) | Pressure delta detection |
| Crypto | CryptoCell-310 (on-die) | ECDSA-P256 hardware signing |
| Gateway (planned) | Raspberry Pi Zero | BLE scanner + MQTT broker |

---

## Firmware Stack

- **RTOS:** Zephyr 4.3.99 via Nordic NCS v3.2.0-rc1
- **Board target:** `nrf52840_mdk_usb_dongle/nrf52840`
- **Build tool:** `west`
- **Crypto driver:** Oberon PSA (software) via `CONFIG_PSA_CRYPTO_DRIVER_OBERON=y`
  - Note: `CONFIG_NRF_CC3XX_PLATFORM` is commented out — CryptoCell HW not yet wired, Oberon fills in
- **Persistent key storage:** `CONFIG_SECURE_STORAGE=y` (Zephyr Secure Storage subsystem, NVS-backed)

---

## Repository Layout

```
lima-node/
├── firmware/               # Zephyr firmware (nRF52840)
│   ├── src/
│   │   ├── main.c          # HW init, sensor thread, FSM thread, HAL trampolines
│   │   ├── fsm.c/h         # Full FSM — states, transitions, work-queue timers
│   │   ├── crypto.c/h      # PSA ECDSA-P256 — init, build_payload, sign_async
│   │   ├── ble.c/h         # BLE non-connectable advertising of lima_payload_t
│   │   └── events.h        # Event enum + lima_event_t struct
│   ├── prj.conf            # Kconfig — all feature flags here
│   ├── Kconfig             # Project-level Kconfig definitions
│   └── CMakeLists.txt
├── gateway/                # Rust BLE gateway (WIP)
│   └── src/main.rs         # btleplug BLE scanner + LIMA payload decoder
├── docs/
│   ├── architecture/       # PlantUML diagrams + rendered PNG/SVG
│   │   └── adr/            # ADR-001 through ADR-006
│   ├── build/              # Build quickref, flash guides
│   ├── dev/                # This file + quickref.md
│   ├── logs/               # Captured minicom debug sessions (.cap files)
│   └── verification/       # I2C, signal integrity, setup notes
├── artifacts/              # Pinouts, specsheets, bootloader files
└── west.yml                # NCS workspace manifest
```

---

## What Is Done (Verified, Merged to Main)

### Firmware

- **FSM pipeline** — Full `BOOT → CALIBRATING → ARMED → EVENT_DETECTED → SIGNING → TRANSMITTING → COOLDOWN` cycle validated end-to-end (~366µs stub latency in early test)
- **Dual-thread model** — `sensor_thread` (priority 6, 60ms poll) + `fsm_thread` (priority 5, message queue) decoupled cleanly
- **IMU driver** — MPU6050 via Zephyr I2C, motion threshold at 0.80g, I2C bus recovery on EIO
- **Barometric driver** — BME280 via Zephyr I2C, 5 Pa default threshold, slow-drift baseline tracking (0.01 alpha), I2C recovery on EIO
- **Dual-trigger logic** — PRESSURE_BREACH and MOTION_DETECTED fire independently as OR triggers
- **Sleep cycle** — `ARMED → (armed_dwell_work) → LIGHT_SLEEP → (inactivity_work 30s) → DEEP_SLEEP → (rtc_wakeup_work stub) → ARMED` validated
- **LED state machine** — distinct patterns for each FSM state (white=boot, blue double-blink=armed, red=alert pipeline, yellow=cooldown, red+blue pulse=light sleep, white ghost-pulse=deep sleep)
- **Crypto (ECDSA-P256/SHA-256)** — signing working, ~107ms via PSA API
  - Persistent key: `CONFIG_SECURE_STORAGE=y`, key survives power cycles
  - Key ID configured via `CONFIG_LIMA_CRYPTO_KEY_ID`
  - On first boot: generates key, persists it. On subsequent boots: loads existing key
  - Public key exported and logged at INF level on init (for gateway registration)
- **BLE advertising** — `LIMA_NODE_01` non-connectable ADV_NONCONN_IND broadcasting `lima_adv_payload_t` (manufacturer-specific AD type, company_id=0xFFFF)
  - Verified on nRF Connect at -52 dBm RSSI
  - 100–150ms adv interval, duration configured via `CONFIG_LIMA_BLE_ADV_DURATION_MS`
  - `adv_stop_work` schedules BLE stop and fires `LIMA_EVT_TX_COMPLETE` callback

### Gateway

- **Rust scaffold** — `gateway/src/main.rs` with `btleplug` BLE scanner
- **LIMA filter** — scans all peripherals, filters on `local_name.starts_with("LIMA")`
- **Payload decoder** — `decode_lima_payload()` parses raw manufacturer bytes into: `node_id[6]`, `event_type`, `sequence` (u32 LE), `timestamp_ms` (u32 LE), `accel_g` (f32 LE), `delta_pa` (f32 LE)
- **Dependencies:** `btleplug 0.11`, `tokio 1 (full)`, `ratatui 0.26`, `crossterm 0.27`, `serde 1`

---

## What Is NOT Done (Next Up)

### Gateway (Active Focus)

These are the next concrete tasks in priority order:

1. **MQTT publisher** — pipe decoded LIMA events to local Mosquitto broker (topic: `lima/node/<id>/event`)
2. **Mosquitto broker config** — local broker setup on Pi Zero, listener + ACL
3. **SQLite audit log** — write every event to DB before publishing (audit trail survives MQTT outages)
4. **Queue-and-flush egress** — buffer events if broker unreachable, flush on reconnect
5. **Push notifications** — Pushover or Pushbullet handler for operator alerting
6. **TUI dashboard** — `ratatui` dependency is already declared, not wired

### Firmware (Deferred)

- **ECDSA-P256 encryption** — currently only signing, no payload encryption
- **Node ID fix** — `node_id` is still hardcoded as `0xDE:AD:BE:EF:00:01`; needs `bt_id_get()` to use real BLE MAC (blocked until BLE stack is confirmed stable across crypto+BLE init order)
- **Sequence counter persistence** — counter resets to 0 on every boot; needs NVS write on increment (minor, same NVS subsystem as key storage)
- **Real power management** — `hw_enter_light_sleep()` and `hw_enter_deep_sleep()` are stubs; real `PM_STATE_SUSPEND_TO_IDLE` and `PM_STATE_SOFT_OFF` not wired; sleep is currently software delay + RTC wakeup stub (`k_work_reschedule`)
- **Watchdog** — wdt is disabled on boot (`wdt_disable(wdt)`); needs proper kick in FSM loop

---

## Key Data Structures

### `lima_payload_t` (24 bytes, signed)
```c
uint8_t  node_id[6];     // BLE MAC (currently hardcoded DEADBEEF:0001)
uint8_t  event_type;     // lima_event_type_t
uint8_t  reserved;
uint32_t sequence;       // monotonic counter (resets on boot — NVS TODO)
uint32_t timestamp_ms;   // k_uptime_get_32()
float    accel_g;        // peak acceleration (IMU events)
float    delta_pa;       // pressure delta (baro events)
```

### `lima_adv_payload_t` (BLE manufacturer data)
```c
uint16_t company_id;     // 0xFFFF (test/dev)
uint8_t  proto_version;  // 0x01
uint8_t  event_type;
uint32_t sequence;
uint32_t timestamp_ms;
float    accel_g;
float    delta_pa;
uint8_t  node_id[6];
```

### FSM States
```
BOOT → CALIBRATING → ARMED ⟷ LIGHT_SLEEP ⟷ DEEP_SLEEP
                      ↓
               EVENT_DETECTED → SIGNING → TRANSMITTING → COOLDOWN → ARMED
                                                ↓              ↓
                                              FAULT ←──────────┘
```

---

## Build Quick Reference

```bash
# Linux — build
west build -b nrf52840_mdk_usb_dongle lima-node/firmware --pristine -- -DCONFIG_BUILD_OUTPUT_UF2=y

# Flash (dongle in bootloader mode)
cp build/firmware/zephyr/zephyr.uf2 /media/$USER/UF2BOOT

# Serial console
screen /dev/ttyACM0
# or with capture:
minicom -D /dev/ttyACM0 -C debug-$(date +%H%M%S).cap

# macOS equivalents
cp build/firmware/zephyr/zephyr.uf2 /Volumes/UF2BOOT
screen /dev/tty.usbmodem
```

---

## Key Configuration (prj.conf)

| Config | Value | Notes |
|---|---|---|
| `CONFIG_LIMA_BARO_THRESHOLD_PA` | 5 | Pa delta to trigger pressure event |
| `CONFIG_LIMA_DEEP_SLEEP_INTERVAL_MS` | 6000 | RTC wakeup stub interval |
| `CONFIG_SECURE_STORAGE` | y | Persistent PSA key storage |
| `CONFIG_PSA_CRYPTO_DRIVER_OBERON` | y | Software PSA driver |
| `CONFIG_BT_DEVICE_NAME` | "LIMA_NODE_01" | BLE advertised name |
| `CONFIG_NRF_CC3XX_PLATFORM` | (commented out) | HW crypto not yet enabled |

Thresholds (in `main.c`):
- `MOTION_THRESHOLD_G` = 0.80g
- `POLL_INTERVAL_MS` = 60ms (16.67 Hz)
- `COOLDOWN_MS_DEFAULT` = 5000ms
- `FSM_STACK_SIZE` = 8192
- `SENSOR_STACK_SIZE` = 4096

---

## Architecture Decision Records

| ADR | Decision | Status |
|---|---|---|
| ADR-001 | nRF52840 over ESP32 / STM32 | Active |
| ADR-002 | BLE 5.0 over Thread / Zigbee / LoRa | Active |
| ADR-003 | MQTT over CoAP / raw TCP / HTTP | Active |
| ADR-004 | Zephyr RTOS over bare metal / FreeRTOS | Active |
| ADR-005 | AES-256-GCM + ECDSA-P256 on all payloads | Active |
| ADR-006 | Persistent PSA Key Storage | **Resolved** — `CONFIG_SECURE_STORAGE=y` implemented in `feat(crypto)` PR #4 |

---

## Git History Summary (Significant Commits)

```
43f2a4a  test(fsm): capture logs — sensor signing + persistent keys (current branch head)
b6c1b0b  feat(crypto): persistent ECDSA-P256 via Zephyr Secure Storage (PR #4, merged)
f0127c8  feat(gw): BLE gateway receiver with LIMA payload decoder (PR #3, merged)
a31476f  docs(readme): narrative flow reorganization (PR #2, merged)
3b190bd  feat(gateway): init Rust gateway scaffold
8dfd9b4  feat(ble): LIMA_NODE_01 advertising live — verified on nRF Connect
7b38e52  test(crypto): confirm signing for each trigger + full state cycle
b5169ca  feat(crypto): add single-key crypto signing to telemetry packet
f010029  test(fsm): full sleep cycle validated (ARMED/LIGHT/DEEP/RTC loop)
b3a54cf  feat(fsm): full sensor-driven pipeline + correct inactivity timer reset
b2c8a1b  feat(sensors): wire BME280 pressure reads into FSM event pipeline
```

---

## Known Issues / Watch Out For

1. **prj.conf has comment noise** — lots of commented-out alternative storage configs from the persistent key exploration. The active config is `CONFIG_SECURE_STORAGE=y` at the bottom. The commented-out `CONFIG_TRUSTED_STORAGE`, `CONFIG_PSA_ITS_FLASH`, etc. are dead branches from previous attempts.

2. **`lima_crypto_sign_async()` is synchronous** — despite the name. The `_async` suffix was aspirational; it calls `psa_sign_message()` inline and fires the callback immediately. Suitable for current use but blocks the FSM thread for ~107ms during signing.

3. **BLE init order matters** — `bt_enable(NULL)` must precede `lima_ble_init()` and `lima_crypto_init()` must precede `fsm_init()`. Order in `main()` is correct and tested.

4. **`sensor_thread` polls even in DEEP_SLEEP** — the sensor thread checks `fsm_get_state()` and skips reads when not `ARMED` or `LIGHT_SLEEP`, but the thread still wakes up every 60ms. Real deep sleep will require thread suspension.

5. **Watchdog disabled** — `wdt_disable(wdt)` in `main()`. Intentional for dev. Re-enable before production.

6. **Gateway `node_id` byte order** — gateway decoder assumes `node_id` at bytes 0–5 in manufacturer data. Confirm byte ordering matches `lima_adv_payload_t` layout when wiring MQTT.

---

## Next Session Starting Point

The gateway is the active development front. The firmware is feature-complete for the current milestone (signing + BLE advertising + persistent keys). The next concrete step is wiring MQTT publishing into `gateway/src/main.rs` — the BLE scan loop is already working and filtering LIMA PDUs.

Suggested next tasks:
1. Add `paho-mqtt` or `rumqttc` crate to `gateway/Cargo.toml`
2. Publish decoded `lima_adv_payload_t` to `lima/node/{node_id}/event` topic as JSON
3. Add `rusqlite` for local audit log
4. Wire `ratatui` TUI for live event display (dependency already declared)
