# LIMA Gateway — Development Context

**Date:** 2026-03-15  
**Branch:** feature/gateway-pubkey  
**Status:** verify_outer_sig() being integrated — green checkmarks imminent  

---

## Architecture Summary

The Rust gateway runs on Raspberry Pi 5. It scans for BLE extended ADV
packets from LIMA nodes, verifies the outer ECDSA-P256 signature, logs
to SQLite, and displays results in a ratatui TUI.

The gateway operates at the **outer signature verification layer only**.
Event type, sensor data, and payload semantics are inside the AES-256-GCM
encrypted inner payload — opaque until the AES decrypt sprint (v0.3).

---

## What the Gateway Actually Knows

From a received 90-byte ADV blob the gateway can determine:

| Field         | Available now? | Notes                                      |
|---------------|----------------|--------------------------------------------|
| node_id       | ✅             | bytes[20..26] in ADV layout                |
| sig_verified  | ✅             | ECDSA-P256 over reconstructed lima_payload_t |
| raw_blob      | ✅             | full 90 bytes stored as hex                |
| rssi          | ✅             | from btleplug advertisement metadata       |
| received_at   | ✅             | system timestamp at receipt                |
| event_type    | ❌             | inside encrypted payload — opaque          |
| sequence      | ⚠️             | bytes[4..8] in ADV layout — available but not yet decoded |
| accel_g       | ❌             | inside encrypted payload                   |
| delta_pa      | ❌             | inside encrypted payload                   |

---

## verify_outer_sig() — Correct Implementation

The gateway must reconstruct `lima_payload_t` field order before verifying.
Field order differs between `lima_payload_t` and `lima_adv_payload_t` —
`node_id` is at offset 0 in the signed struct but offset 20 in the ADV struct.

```rust
fn verify_outer_sig(payload: &[u8], vk: &VerifyingKey) -> bool {
    const ADV_LEN:     usize = 90;
    const SIG_LEN:     usize = 64;
    const PAYLOAD_LEN: usize = 24;

    if payload.len() < ADV_LEN {
        return false;
    }

    // Extract fields from ADV layout
    let event_type   = payload[3];
    let sequence     = &payload[4..8];
    let timestamp_ms = &payload[8..12];
    let accel_g      = &payload[12..16];
    let delta_pa     = &payload[16..20];
    let node_id      = &payload[20..26];
    let sig_bytes    = &payload[26..90];

    // Reconstruct lima_payload_t byte order (what was actually signed)
    let mut signed_data = [0u8; PAYLOAD_LEN];
    signed_data[0..6].copy_from_slice(node_id);
    signed_data[6]    = event_type;
    signed_data[7]    = 0x00;                    // reserved
    signed_data[8..12].copy_from_slice(sequence);
    signed_data[12..16].copy_from_slice(timestamp_ms);
    signed_data[16..20].copy_from_slice(accel_g);
    signed_data[20..24].copy_from_slice(delta_pa);

    match Signature::from_slice(sig_bytes) {
        Ok(sig) => vk.verify(&signed_data, &sig).is_ok(),
        Err(_)  => false,
    }
}
```

---

## event_type in DB and TUI

Since event_type is inside the encrypted payload, the DB and TUI should
show `--` or `encrypted` for this field until the AES decrypt sprint.

```rust
// BLE handler — event_type extraction
let event_type = "encrypted".to_string(); // opaque until AES sprint
```

The `raw_blob_hex` column is the source of truth — all 90 bytes stored,
fully recoverable for future decryption passes.

---

## DB Schema

```sql
CREATE TABLE IF NOT EXISTS events (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    node_id       TEXT    NOT NULL,       -- E3:79:63:12:EF:B1
    received_at   INTEGER NOT NULL,       -- unix timestamp ms
    sig_verified  INTEGER NOT NULL,       -- 0 or 1
    event_type    TEXT    NOT NULL,       -- "encrypted" until AES sprint
    rssi          INTEGER NOT NULL,       -- dBm
    raw_blob      BLOB    NOT NULL        -- 90 bytes hex — source of truth
);
```

No schema migration needed — `raw_blob` is a BLOB, takes any size.

---

## Node Public Key

Hardcoded in `main.rs` for v0.1.0. In production this will come from
a provisioning registry.

```rust
const TEST_NODE_PUBKEY_HEX: &str = concat!(
    "04 8d a8 7d 0a 4d df c4  16 c4 01 82 6e d8 ea 0d ",
    "b2 9e c3 65 13 50 69 69  b8 8c 83 79 de 06 e3 10 ",
    "3e 42 a3 9e 66 e8 f3 e7  aa 62 d2 aa 24 18 4d 88 ",
    "e1 1f 2c 7a aa 9d e8 a0  48 84 90 5b 59 ed 48 7f ",
    "d7"
);
```

Node identity: `E3:79:63:12:EF:B1`  
Key slot: `0x00000001`  
Algorithm: ECDSA-P256 / SHA-256 (CryptoCell-310 hardware accelerated)

---

## TUI Layout

```
┌─ LIMA Gateway | rx: N  valid: N  invalid: N ──────────────────────────────┐
│                                                                             │
│ Events (newest first)                                                       │
│ time        node_id            type        seq   sig          rssi          │
│ HH:MM:SS    E3:79:63:12:EF:B1  encrypted   --    ✓ VALID      -XX dBm      │
│                                                                             │
├─────────────────────────────────────────────────────────────────────────────┤
│ q: quit | DB: lima_gateway.db | last sig: ✓ VALID 5e7f68...| no AES yet   │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## Open Work Items

- [ ] `verify_outer_sig()` — update to reconstruct `lima_payload_t` layout
- [ ] `event_type` field — set to `"encrypted"` in BLE handler
- [ ] `seq` field in TUI — decode from bytes[4..8] or show `--`
- [ ] Green checkmarks — confirm once verify_outer_sig() lands
- [ ] MQTT pipeline — route verified events to broker (v0.2 target)
- [ ] AES-256-GCM inner decrypt (v0.3 target)
- [ ] Node registry — replace hardcoded pubkey with provisioning DB

---

## Crate Workspace

```
lima-ws/
├── gateway/crates/
│   ├── gateway/        — main binary, BLE scan, TUI, SQLite
│   ├── lima-types/     — shared types (LimaPayload, wire constants)
│   ├── crypto-test/    — end-to-end encrypt/sign/verify test harness
│   └── lima-web/       — placeholder
```

---

*Context written 2026-03-15. Pick up from: update verify_outer_sig() and event_type, then confirm green checkmarks on TUI.*
