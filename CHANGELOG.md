# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.1.0] - 2026-XX-XX

Initial release. Single-node reference implementation of the LIMA protocol —
authenticated physical integrity monitoring for ICS/OT edge nodes.

### Added

**Node firmware — nRF52840 / Zephyr (NCS v3.2.2)**

- Event-driven FSM with 11 states; ISRs and sensor threads post `lima_event_t`
  to a message queue consumed by the FSM thread
- Sensor support: MPU6050 (motion), BME280 (environmental), DS3231
  (battery-backed RTC with NVS epoch backup and tamper-epoch anchor)
- LIMA Event Record (LER) — 24-byte attested payload
- LIMA Frame (LF) — 184-byte wire format:
  4B header · 12B nonce · 88B ciphertext · 16B GCM tag · 64B outer signature
- Encrypt-then-sign construction via PSA:
  - Inner ECDSA-P256 signature over the LER plaintext
  - AES-256-GCM encryption of `LER || inner_sig`, LF header as AAD
  - Outer ECDSA-P256 signature over LF bytes 0–120
  - Plaintext zeroed from the stack immediately after encryption
- Hardware-accelerated crypto via CryptoCell-310
- BLE 5.0 extended advertising transport
- Board support: `nrf52840dk/nrf52840` and nRF52840 MDK USB dongle

**Gateway — Rust**

- Blind relay: stores encrypted frames without holding node key material
- SQLite audit log
- MQTT publish (`rumqttc`)
- ntfy push notification dispatch
- `ratatui` TUI
- Workspace crates: `lima-types`, `crypto-test`, `gateway`

**Client**

- TUI client with live AES-256-GCM decrypt and local SQLite store

### Known limitations

- **Power management is not implemented.** `CONFIG_PM` is unsupported on this
  board; only `CONFIG_PM_DEVICE` is enabled. `DEEP_SLEEP` transitions and wakes
  correctly on a DS3231 alarm, but `hw_ble_stop()` is a stub and the SoC never
  leaves run mode — the state machine is correct over absent power management.
  Deferred pending mainboard power testing.
- **Single node only.** No multi-node fleet orchestration, enrollment, or key
  rotation. Node keys are provisioned manually.
- **Pre-shared symmetric key.** Ephemeral ECDH key agreement requires an LF
  spec change and is deferred.

[Unreleased]: https://github.com/jknoxdev/lima-node/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/jknoxdev/lima-node/releases/tag/v0.1.0
