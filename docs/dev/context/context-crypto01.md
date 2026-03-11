# LIMA Crypto Architecture — Context Document
# context-crypto01.md
# Status: Decided — ready for implementation
# Date: 2026-03-10

---

## 1. Scope

This document captures all cryptographic architecture decisions made for LIMA v1.0
covering the node firmware (nRF52840 / Zephyr), the gateway (Rust / Pi 5), and the
verification test utility. It is the authoritative reference for the crypto sprint.

---

## 2. Curve Selection

**Decision: NIST P-256 (secp256r1)**

- `p256` crate (RustCrypto) on the gateway / crypto-test / lima-web
- PSA Crypto API (`PSA_ALG_ECDSA(PSA_ALG_SHA_256)`) on the nRF52840 firmware
- CryptoCell-310 has **native hardware acceleration for P-256** — this is the
  primary driver. secp256k1 (`k256`) was considered but rejected because the
  nRF CryptoCell-310 does not accelerate it, losing hardware signing performance
  and increasing power consumption per event.
- Note: `k256` and `p256` are both from the RustCrypto family and have near-identical
  APIs. `k256` was initially suggested in error — `p256` is correct.

---

## 3. Key Agreement

**Decision: ECDH (Elliptic Curve Diffie-Hellman) over P-256**

- Each node generates a P-256 keypair on first boot via CryptoCell-310
- Gateway generates its own P-256 keypair
- Shared secret = ECDH(node_privkey, gateway_pubkey) == ECDH(gateway_privkey, node_pubkey)
- Shared secret is never transmitted — derived independently on each side
- Shared secret → HKDF-SHA256 → AES-256-GCM session key

**Rationale over PSK:**
- Scales to N nodes without a shared secret database
- Adding a node = registering its public key in the gateway trust store (SQLite)
- Attack surface is explicitly bounded to the provisioning step (key registration)
- Same model as WireGuard, SSH host keys, Signal X3DH

**Attack surface acknowledgement:**
The key registration / provisioning ceremony is the trust establishment point.
Everything after provisioning is cryptographically protected. This must be
documented and hardened for production deployments (out of scope for v1.0).

---

## 4. Encryption

**Decision: AES-256-GCM**

- Symmetric encryption using HKDF-derived key (see §3)
- 12-byte nonce (random, per-event)
- 16-byte GCM authentication tag
- Plaintext input: `lima_payload_t` (24B) || ECDSA signature (64B) = 88B
- Ciphertext output: 88B ciphertext + 16B tag = 104B

---

## 5. Signing

**Decision: ECDSA-P256 with SHA-256**

- Node signs using its provisioned private key (stored in PSA persistent key slot)
- Signature is 64 bytes (r || s, raw format)
- Gateway verifies using the node's registered public key from trust store

---

## 6. Wire Format (Encrypt-then-Sign)

**Decision: Encrypt-then-Sign**

Rationale: Gateway can verify the ECDSA signature before performing any AES
decryption work. Forged or replayed advertisements are rejected cheaply at the
signature check — no AES operations wasted on garbage input. This is the
correct posture for an edge device (Pi 5) serving potentially many nodes.

```
BLE Manufacturer-Specific Payload (~120B total):

 ┌──────────┬──────────┬────────────────────────┬──────────┬─────────────┐
 │ header   │ nonce    │ ciphertext             │ GCM tag  │ ECDSA sig   │
 │ 4B       │ 12B      │ 88B                    │ 16B      │ 64B         │
 └──────────┴──────────┴────────────────────────┴──────────┴─────────────┘
                        ←──── AES-256-GCM ────→
            ←──────────────── signed by ECDSA-P256 ────────────────────→

Plaintext inside ciphertext:
 ┌────────────────────────┬─────────────┐
 │ lima_payload_t  24B    │ ECDSA sig   │  ← this is what gets encrypted
 └────────────────────────┴─────────────┘
```

**Node operation order:**
1. Build `lima_payload_t` (24B)
2. ECDSA-P256 sign payload → 64B signature
3. Concatenate: payload || sig → 88B plaintext
4. AES-256-GCM encrypt(88B) → ciphertext (88B) + tag (16B), generate nonce (12B)
5. Concatenate: header || nonce || ciphertext || tag → sign with ECDSA
6. Advertise full packet over BLE extended advertisement

**Gateway operation order:**
1. Scan BLE advertisement, extract manufacturer payload
2. ECDSA-P256 verify outer signature → reject if invalid (cheap, no AES work)
3. AES-256-GCM decrypt ciphertext using HKDF-derived key
4. Split plaintext → `lima_payload_t` (24B) + inner sig (64B)
5. (Optional deeper verify) re-verify inner sig against payload
6. Store raw encrypted blob in SQLite (gateway never needs to store plaintext)
7. Update ratatui TUI — node ID, sig status ✅/❌, sequence, timestamp

---

## 7. Gateway Storage Model

**Decision: Store encrypted blobs, never plaintext**

The gateway stores the raw encrypted payload as-is in SQLite. The gateway only
needs the node's public key (for sig verification) — it never holds the AES key.

**Rationale:**
- Gateway is the most physically exposed component (on-site hardware)
- If gateway is compromised, attacker gets signed+encrypted blobs — unreadable
  without the ECDH-derived key
- Decryption happens in a separate utility (`crypto-test`) or the lima-web app,
  which can run on a more trusted host

**SQLite schema (gateway):**
```sql
CREATE TABLE events (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    node_id       TEXT NOT NULL,           -- hex BLE MAC
    received_at   INTEGER NOT NULL,        -- unix timestamp ms
    sequence      INTEGER,                 -- from outer header if present
    sig_verified  INTEGER NOT NULL,        -- 1 = valid, 0 = rejected
    raw_blob      BLOB NOT NULL            -- full encrypted manufacturer payload
);
```

---

## 8. Rust Crate Dependencies

```toml
# Crypto
p256    = { version = "0.13", features = ["ecdsa", "ecdh"] }
aes-gcm = "0.10"
hkdf    = "0.12"
sha2    = "0.10"
rand_core = { version = "0.6", features = ["getrandom"] }

# Gateway
btleplug  = "0.11"
rusqlite  = { version = "0.31", features = ["bundled"] }
ratatui   = "0.26"
crossterm = "0.27"

# Web
axum      = "0.7"
rumqttc   = "0.24"

# Shared
tokio     = { version = "1", features = ["full"] }
serde     = { version = "1", features = ["derive"] }
serde_json = "1"
hex       = "0.4"
```

---

## 9. Workspace Structure

```
lima-gateway/
  Cargo.toml            ← workspace root
  crates/
    lima-types/         ← shared: LimaPayload, LimaEvent, wire format constants
      src/lib.rs
    gateway/            ← BLE scanner + sig verify + SQLite + ratatui TUI
      src/main.rs
    crypto-test/        ← end-to-end round-trip test utility (build first)
      src/main.rs
    lima-web/           ← Axum + MQTT subscriber + decrypt + web UI (next sprint)
      src/main.rs
```

---

## 10. Build Order (This Sprint)

```
1. Cargo workspace scaffold
2. lima-types crate — LimaPayload struct, wire format constants
3. crypto-test binary — full round-trip: keygen → ECDH → HKDF → encrypt →
                        sign → verify → decrypt → assert → hex dump each step
4. gateway Cargo.toml deps wired
5. ratatui TUI skeleton
```

`lima-web` is **out of scope for this sprint.**

---

## 11. Open Items

- [ ] Provisioning ceremony design — how node pubkey gets registered on gateway
      (v1.0: manual `gateway register-node --pubkey <hex>` CLI command)
- [ ] Nonce management — random per-event is correct for v1.0, sequence-based
      nonces considered for anti-replay hardening in v1.1
- [x] Inner signature — KEEP. Outer sig = PDU authenticity (this node sent this
      packet). Inner sig = payload authenticity (this node built this payload).
      Independent verification layers — MitM stripping outer layer still caught
      by inner sig. Defense in depth, two distinct trust boundaries.
- [ ] Key rotation — out of scope v1.0, noted in FUTURE.md
- [x] HKDF context string — decided (32 random bytes, no semantic content):
      Rust:    b"\x2e\x29\xee\x16\xe8\x10\x6d\x8a\xdd\xbb\x50\xe2\x12\x16\x3d\xfd\xa8\xf4\x24\xe2\xc9\x7d\x4b\xd3\x17\xb9\x9a\x96\xe0\x7e\x5c\x6f"
      C/hex:   2e29ee16 e8106d8a ddbb50e2 12163dfd a8f424e2 c97d4bd3 17b99a96 e07e5c6f
      Must be byte-identical in firmware (crypto.c) and gateway (crypto-test/main.rs)

---

## 12. ADR References

- ADR-005: AES-256-GCM + ECDSA-P256 target — covers algorithm selection
- ADR-006: BLE milestone prioritized — covers sequencing rationale
- Encrypt-then-Sign ordering is implicit in ADR-005, no separate ADR required
- ECDH key agreement rationale captured here (context-crypto01.md) pending
  possible ADR-007 if needed for conference documentation
