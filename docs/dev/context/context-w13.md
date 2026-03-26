# LIMA — Week 13 Context

> **Created:** 2026-03-26
> **Branch:** main (post-PR #16)
> **Purpose:** State-of-the-union and recommended next action heading into week 13.

---

## Where We Are

### Firmware (nRF52840) — largely complete for v0.1

| Subsystem | Status |
|---|---|
| FSM pipeline | ✅ validated end-to-end |
| ECDSA-P256 signing (CryptoCell-310) | ✅ ~107ms, hardware accelerated |
| BLE ext ADV with signed payload | ✅ verified on nRF Connect |
| MPU6050 + BME280 sensors | ✅ OR trigger logic, I2C bus recovery |
| DS3231 RTC module (`rtc.c` / `rtc.h`) | ✅ extracted PR #15, provisioning PR #16 |
| Three-tier epoch fallback (HW → NVS → Kconfig) | ✅ |
| RTC tamper skew detection | ✅ |
| PM_STATE_SOFT_OFF (true hardware deep sleep) | ❌ not yet wired |
| ECDSA-P256 **encryption** (roadmap item) | ❌ not started |
| Sensor timestamps using wall-clock RTC | ⚠️ still `k_uptime_get_32()` at main.c:535,545 |

### Gateway (Rust / Pi) — BLE→SQLite path validated

| Subsystem | Status |
|---|---|
| btleplug BLE extended ADV scanner | ✅ PR #13 |
| ECDSA-P256 outer signature verification | ✅ ✅ VALID confirmed |
| SQLite audit log (`lima_gateway.db`) | ✅ schema + insert working |
| ratatui TUI (live event table) | ✅ |
| MQTT pipeline (broker + publisher) | ❌ not started |
| Queue-and-flush egress | ❌ not started |
| Pushover / Pushbullet notification handler | ❌ not started |
| AES-256-GCM inner payload decrypt | ❌ v0.3 target |
| Node registry (replace hardcoded pubkey) | ❌ v0.3 target |

---

## Recommended Next Action: Gateway MQTT Pipeline

The BLE → SQLite path is validated and clean. The natural next sprint is the
**MQTT pipeline** — this is the highest-leverage unblocked item and directly
matches the current phase stated in the README:

> *"Gateway receiver + MQTT + SQLite audit log"*

The SQLite piece is done. MQTT is the gap.

### What to build

**1. Mosquitto broker config** (`gateway/scripts/mosquitto.conf`)

Minimal local broker on `localhost:1883`, no TLS for v0.1 (air-gapped LAN).
Topic structure: `lima/events/<node_id>`.

**2. MQTT publisher crate** (`gateway/crates/mqtt/` or inline in `gateway`)

- Dependency: `rumqttc` (async, fits the existing tokio runtime)
- Publish on every verified event (sig_verified == true AND sig_verified == false — failed verifications are security events too)
- JSON payload mirroring the DB row: `node_id`, `received_at`, `sig_verified`, `rssi`, `raw_blob_hex`

**3. Queue-and-flush egress**

- If MQTT publish fails (broker down, network gone), buffer to a `VecDeque` in `App`
- On reconnect, drain queue before processing new events
- Cap queue at ~1000 events to bound memory; log overflow

**4. Notification hook** (stretch goal for this sprint)

- Subscribe to `lima/events/#` in a second task
- On `sig_verified == false`: fire Pushover API call
- On `sig_verified == true` with `event_type != "encrypted"` (future): same
- For now, alert on any invalid signature as a tamper event

### Minimal wiring sketch

```rust
// In ble_task — after db_insert:
if let Err(e) = mqtt_client.publish(
    format!("lima/events/{}", rec.node_id),
    QoS::AtLeastOnce,
    false,
    serde_json::to_vec(&rec)?,
).await {
    app.lock().await.mqtt_queue.push_back(rec.clone());
    eprintln!("MQTT publish failed: {}", e);
}
```

---

## Firmware Housekeeping (small — do before or alongside MQTT sprint)

These are leftover items from the RTC refactor (context-rtc-refactor.md):

1. **Sensor timestamps** — replace `k_uptime_get_32()` at `main.c:535` and `main.c:545`
   with `(uint32_t)lima_rtc_timestamp_ms()`. One-liner each. Payloads will then carry
   real wall-clock time instead of uptime ticks.

2. **PM_STATE_SOFT_OFF** — after `lima_rtc_arm_wakeup()` arms the DS3231 alarm, call
   `pm_state_force()` and configure the INT/SQW pin as a GPIO wakeup source via
   `nrf_gpio_cfg_sense_set()`. This is the last step to true low-power operation.
   Can be deferred until after MQTT sprint since the software sleep fallback is functional.

---

## Sprint Sequence Suggestion

| Order | Item | Location | Size |
|---|---|---|---|
| 1 | Fix sensor timestamps (`k_uptime_get_32` → `lima_rtc_timestamp_ms`) | `firmware/src/main.c:535,545` | 30 min |
| 2 | Add `rumqttc` dep + `mqtt_task` in gateway | `gateway/crates/gateway/` | ~4h |
| 3 | Mosquitto config + startup script | `gateway/scripts/` | 1h |
| 4 | Queue-and-flush in `App` | `gateway/crates/gateway/src/main.rs` | 2h |
| 5 | Pushover notification task (stretch) | `gateway/crates/gateway/` | 2h |
| 6 | Wire PM_STATE_SOFT_OFF in firmware | `firmware/src/rtc.c` | 3h |

---

## Roadmap Checkpoint

```
- [X] Firmware: IMU + barometric sensor drivers
- [X] Firmware: Event aggregator / OR trigger logic
- [X] Firmware: CryptoCell-310 ECDSA-P256 signing
- [ ] Firmware: CryptoCell-310 ECDSA-P256 encryption        ← v0.3
- [X] Firmware: BLE advertisement with signed payload
- [X] Gateway: BLE scanner + SQLite audit log               ← done (PRs #12–#13)
- [ ] Gateway: Mosquitto broker + MQTT publisher            ← THIS SPRINT
- [ ] Gateway: Queue-and-flush egress                       ← THIS SPRINT
- [ ] Gateway: Pushover / Pushbullet notification handler   ← THIS SPRINT (stretch)
- [ ] Hardware: KiCad schematic
- [ ] Hardware: Power budget / battery model
- [ ] Docs: Threat model diagram
- [ ] Docs: Deployment guide
```

---

*Pick up from: fix sensor timestamps in main.c, then start `mqtt_task` in gateway.*
