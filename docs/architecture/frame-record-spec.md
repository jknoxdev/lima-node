# LIMA Frame / Event Record — Wire Format Spec
**Branch:** `feat/frame-record-spec`  
**Status:** Source of truth — all code derives from this doc  

---

## Terminology

| Abbreviation | Full Name           | C type        | Rust type         | Size    |
|--------------|---------------------|---------------|-------------------|---------|
| LER          | LIMA Event Record   | `lima_ler_t`  | `LimaEventRecord` | 24 B    |
| LF           | LIMA Frame          | `lima_lf_t`   | `LimaFrame`       | 184 B   |

---

## LER — LIMA Event Record (24 bytes)

Inner plaintext struct. Signed by node ECDSA-P256 key. Never transmitted in plaintext — always encrypted inside LF.

```
Offset  Size  Type      Field           Notes
──────  ────  ────────  ──────────────  ──────────────────────────────────
0       6     u8[6]     node_id         BLE MAC address, big-endian
6       1     u8        event_type      See Event Types table below
7       1     u8        reserved        Always 0x00 — alignment pad
8       4     u32 LE    sequence        Monotonic counter, anti-replay
12      4     u32 LE    timestamp_ms    RTC wall-clock epoch milliseconds
16      4     f32 LE    accel_g         IMU vector magnitude at trigger (g)
20      4     f32 LE    delta_pa        Barometric delta at trigger (Pa)
──────  ────
        24              TOTAL
```

---

## LF — LIMA Frame (184 bytes)

Outer wire envelope. BLE extended advertising manufacturer-specific AD data.  
Encrypt-then-Sign: AES-256-GCM over `(LER || inner_sig)`, then ECDSA-P256 over the full frame header+nonce+ciphertext.

```
Offset  Size  Field         Notes
──────  ────  ────────────  ────────────────────────────────────────────────
0       1     proto_version 0x02
1       1     event_type    Mirrors LER.event_type — for gateway pre-filter
2       2     reserved      0x0000
4       12    nonce         AES-256-GCM IV — random per frame
16      88    ciphertext    AES-256-GCM encrypt(LER 24B || inner_sig 64B)
104     16    gcm_tag       AES-256-GCM authentication tag
120     64    outer_sig     ECDSA-P256 sig over bytes[0..120]
──────  ────
        184               TOTAL
```

### Crypto layering

```
plaintext  =  LER (24B) || inner_sig (64B)          = 88B
ciphertext =  AES-256-GCM-Encrypt(plaintext, nonce) = 88B + 16B tag = 104B
outer_sig  =  ECDSA-P256-Sign(LF[0..120])           = 64B
```

Key derivation: HKDF-SHA256 from ECDH shared secret, info = HKDF_INFO constant (must match firmware and gateway exactly).

---

## Event Types

| Value  | C constant                  | Rust variant          |
|--------|-----------------------------|-----------------------|
| 0x01   | `LIMA_EVT_PRESSURE_BREACH`  | `PressureBreach`      |
| 0x02   | `LIMA_EVT_MOTION_DETECTED`  | `MotionDetected`      |
| 0x03   | `LIMA_EVT_DUAL_BREACH`      | `DualBreach`          |
| 0x04   | `LIMA_EVT_WAKEUP`           | `Heartbeat`           |
| 0xFF   | —                           | `Unknown`             |

---

## System data flow

```
Node (nRF52840)
  LER populated from lima_event_t
  inner_sig = ECDSA-P256-Sign(LER)
  ciphertext = AES-256-GCM-Encrypt(LER || inner_sig, nonce)
  LF assembled and broadcast via BLE 5.0 extended ADV (184B)

Gateway (Pi — passive, no decryption keys)
  BLE scanner receives LF (184B)
  outer_sig verified via node public key
  LF stored raw in SQLite (encrypted, untouched)
  MQTT publish: topic lima/events/[node_id], payload = raw LF bytes

Web Client (browser — holds private key)
  Fetches raw LF from gateway /api/events
  outer_sig verified via node public key (Web Crypto API)
  AES-256-GCM decrypt → recovers LER || inner_sig
  inner_sig verified → LER displayed to operator
  Private key never leaves browser process
```

---

## Size budget

| Layer              | Size  |
|--------------------|-------|
| LER plaintext      | 24 B  |
| Inner ECDSA sig    | 64 B  |
| GCM tag            | 16 B  |
| Nonce              | 12 B  |
| Header             | 4 B   |
| Outer ECDSA sig    | 64 B  |
| **LF total**       | **184 B** |

BLE 5.0 extended ADV supports up to 254 bytes of AD data — 184B fits with 70B headroom.

---

## Files that must stay in sync

| File                                    | Contains              |
|-----------------------------------------|-----------------------|
| `firmware/src/crypto.h`                 | `lima_ler_t`          |
| `firmware/src/ble.h`                    | `lima_lf_t`           |
| `gateway/crates/lima-types/src/lib.rs`  | `LimaEventRecord`, `LimaFrame`, `LimaEventType` |
| `gateway/scripts/lima_rx.py`            | Parser for LF wire format |
| `gateway/scripts/lima_verifier.py`      | Outer sig verifier    |
| `docs/dev/frame-record-spec.md`         | **This file — source of truth** |
