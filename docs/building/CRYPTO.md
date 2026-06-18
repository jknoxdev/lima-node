# L.I.M.A. — Crypto Module Design

**Local Integrity Multi-modal Architecture**  
*Version 1.0 — Earth Edition, March 2026*  
*Justin Knox — [jknoxdev](https://github.com/jknoxdev)*

> *"In 2036, somewhere between Earth and Mars, this is how we signed the tamper events."*

---

## Table of Contents

1. [Overview](#1-overview)
2. [Threat Model](#2-threat-model)
3. [FSM Integration](#3-fsm-integration)
4. [Payload Format](#4-payload-format)
5. [Key Storage](#5-key-storage)
6. [API Design](#6-api-design)
7. [Kconfig Options](#7-kconfig-options)
8. [LED State Reference](#8-led-state-reference)
9. [Future Work](#9-future-work)
10. [FSM Milestone Log](#10-fsm-milestone-log)

---

## 1. Overview

LIMA is an open-source physical intrusion detection node built for ICS/OT environments. Each node runs on an nRF52840 microcontroller with Zephyr RTOS and is designed to detect, sign, and transmit tamper events — motion, pressure breach, or dual-sensor correlation — to a gateway over BLE.

This document describes the cryptographic subsystem introduced in v1 of the LIMA firmware. It covers the signing architecture, payload format, key storage strategy, API design, and integration with the FSM `SIGNING` state.

| Property | Value |
|---|---|
| Target Hardware | nRF52840 MDK USB Dongle (Makerdiary) |
| SDK | nRF Connect SDK v3.2.2 / Zephyr ncs-v3.2.2 |
| Crypto Backend | Oberon PSA Crypto (`oberon-psa-crypto` ncs-v3.2.2) |
| Algorithm | ECDSA over NIST P-256 with SHA-256 |
| Key Storage | Nordic KMU (Key Management Unit) — persistent PSA key |
| Payload Size | 24 bytes (signed), 64 bytes (ECDSA-P256 signature) |
| GitHub | [github.com/jknoxdev/lima-node](https://github.com/jknoxdev/lima-node) |

---

## 2. Threat Model

LIMA nodes are deployed at physical perimeter points in ICS environments — cabinet doors, conduit entry points, panel enclosures. The threat model assumes:

- Physical adversary with direct access to the node hardware
- Network adversary capable of intercepting BLE advertisements
- **Replay attacks** — capturing and retransmitting a legitimate signed event
- **Spoofing** — injecting fabricated tamper events into the gateway
- **Denial of service** — suppressing or delaying tamper event delivery

The crypto module directly addresses **replay** and **spoofing**. DoS and physical destruction of the node are architectural concerns handled at the gateway and deployment level.

---

## 3. FSM Integration

### 3.1 State Flow

The crypto module integrates at the `SIGNING` state in the LIMA FSM. The full alert pipeline is:

```
EVENT_DETECTED → SIGNING → TRANSMITTING → COOLDOWN → ARMED
```

When a sensor breach is detected, the FSM latches the event into `fsm.last_event` and transitions to `EVENT_DETECTED`. The entry function kicks off async signing and transitions immediately to `SIGNING`. The `SIGNING` state waits for `LIMA_EVT_SIGNING_COMPLETE`, then transitions to `TRANSMITTING`.

### 3.2 Current Stub Behavior

As of the `style(leds)` commit, the signing path is a pass-through stub:

```c
state_event_detected_enter()
  → lima_post_event(LIMA_EVT_SIGNING_COMPLETE)  // immediate, no real crypto
  → transition(STATE_SIGNING)
```

The crypto module replaces this stub with a real async PSA signing call. The completion callback posts `LIMA_EVT_SIGNING_COMPLETE` when the CryptoCell operation finishes.

### 3.3 Thread Model

Signing runs on the FSM thread via the PSA Crypto API. The nRF52840 CryptoCell-310 is accessed through the Oberon PSA backend — operations are synchronous at the API level but hardware-accelerated. For v1 this is acceptable. A dedicated `crypto_thread` may be warranted if signing latency impacts event throughput in future deployments.

---

## 4. Payload Format

Every tamper event is serialized into a `lima_payload_t` struct before signing. The struct is **24 bytes**, naturally aligned, and zero-padded.

| Field | Type | Size | Description |
|---|---|---|---|
| `node_id` | `uint8_t[6]` | 6 B | BLE MAC or provisioned device identity |
| `sequence` | `uint32_t` | 4 B | Monotonic counter — anti-replay protection |
| `timestamp_ms` | `uint32_t` | 4 B | `k_uptime_get_32()` at event capture time |
| `event_type` | `uint8_t` | 1 B | `lima_event_type_t` — breach, motion, tamper |
| `accel_g` | `float` | 4 B | IMU vector magnitude at trigger (g-force) |
| `delta_pa` | `float` | 4 B | Barometric delta at trigger (Pascals) |
| `reserved` | `uint8_t[2]` | 2 B | Alignment padding — zero-filled |

The entire 24-byte struct is hashed with SHA-256 and signed with ECDSA-P256. The resulting signature is **64 bytes** (`r ‖ s`, fixed-length encoding). Total wire size: **88 bytes per event**.

### 4.1 Anti-Replay

The `sequence` field is a monotonic `uint32_t` counter stored in retained RAM or flash. It increments on every signed event and is included in the signed payload. The gateway rejects any event with a sequence number less than or equal to the last accepted sequence for that `node_id`.

On cold boot, the sequence counter is restored from flash. If flash read fails, the counter resets to zero and the node posts `LIMA_EVT_SENSOR_FAULT` to trigger investigation.

---

## 5. Key Storage

### 5.1 Nordic KMU

The private signing key is stored in the nRF52840 **Key Management Unit (KMU)**. The KMU provides hardware-isolated key storage with push/revoke semantics — the CPU cannot read the raw key material after provisioning, only instruct the CryptoCell to use it.

| Property | Value |
|---|---|
| PSA Key Lifetime | `PSA_KEY_LIFETIME_PERSISTENT` |
| PSA Key ID | `CONFIG_LIMA_CRYPTO_KEY_ID` (Kconfig, default `0x00000001`) |
| Key Type | `PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1)` |
| Key Bits | `256` |
| Algorithm | `PSA_ALG_ECDSA(PSA_ALG_SHA_256)` |
| Key Usage | `PSA_KEY_USAGE_SIGN_HASH` |

### 5.2 Key Provisioning

For v1 development, the private key is generated on first boot if no persistent key exists at `CONFIG_LIMA_CRYPTO_KEY_ID`. The corresponding public key is exported to flash for gateway-side verification.

Production deployment replaces first-boot generation with factory provisioning — the key is injected at manufacture using `nrfutil` and the KMU push API, and the public key is registered in the gateway's trust store at the same time.

### 5.3 Key Rotation *(Future)*

Key rotation is out of scope for v1. The architecture supports it via PSA key destruction and reprovisioning, triggered by a signed command from the gateway. See `FUTURE.md`.

---

## 6. API Design

### 6.1 `crypto.h`

```c
/* Payload struct — 24 bytes, signed in full */
typedef struct {
    uint8_t  node_id[6];      /* BLE MAC or provisioned ID            */
    uint32_t sequence;        /* monotonic counter — anti-replay       */
    uint32_t timestamp_ms;    /* k_uptime_get_32() at event time       */
    uint8_t  event_type;      /* lima_event_type_t                     */
    float    accel_g;         /* IMU vector magnitude at trigger (g)   */
    float    delta_pa;        /* barometric delta at trigger (Pa)      */
    uint8_t  reserved[2];     /* alignment padding — zero-filled       */
} lima_payload_t;             /* 24 bytes                              */

/* Signing result — passed to completion callback */
typedef struct {
    uint8_t  sig[64];         /* ECDSA-P256 signature (r || s)         */
    size_t   sig_len;         /* always 64 for P-256                   */
    int      err;             /* 0 on success, PSA error code          */
} lima_sig_result_t;

/* Callback type */
typedef void (*lima_sign_cb_t)(const lima_sig_result_t *result);

/* Initialize PSA crypto subsystem and provision key if needed */
int lima_crypto_init(void);

/* Populate payload from FSM context */
void lima_crypto_build_payload(lima_payload_t *payload,
                               const lima_event_t *evt);

/* Sign payload — callback posts LIMA_EVT_SIGNING_COMPLETE on completion */
int lima_crypto_sign_async(const lima_payload_t *payload,
                           lima_sign_cb_t cb);
```

### 6.2 Integration in `fsm.c`

`state_signing_enter()` replaces its stub with:

```c
static void state_signing_enter(void)
{
    LOG_INF("SIGNING: CryptoCell ECDSA-P256 in progress");

    lima_payload_t payload;
    lima_crypto_build_payload(&payload, &fsm.last_event);

    int err = lima_crypto_sign_async(&payload, signing_complete_cb);
    if (err != 0) {
        LOG_ERR("SIGNING: failed to start crypto operation (%d)", err);
        transition(STATE_FAULT);
    }
}

static void signing_complete_cb(const lima_sig_result_t *result)
{
    if (result->err != 0) {
        LOG_ERR("SIGNING: CryptoCell error %d -> FAULT", result->err);
        lima_event_t e = { .type = LIMA_EVT_SENSOR_FAULT,
                           .timestamp_ms = k_uptime_get_32() };
        lima_post_event(&e);
        return;
    }

    LOG_INF("SIGNING: payload signed (%u bytes) -> TRANSMITTING", result->sig_len);
    lima_event_t e = { .type = LIMA_EVT_SIGNING_COMPLETE,
                       .timestamp_ms = k_uptime_get_32() };
    lima_post_event(&e);
}
```

---

## 7. Kconfig Options

| Option | Description |
|---|---|
| `CONFIG_LIMA_CRYPTO` | Enable crypto module (bool, default `y`) |
| `CONFIG_LIMA_CRYPTO_KEY_ID` | PSA persistent key slot ID (hex, default `0x00000001`) |
| `CONFIG_LIMA_NODE_ID_SOURCE_BLE` | Use BLE MAC as `node_id` (bool, default `y`) |
| `CONFIG_LIMA_NODE_ID_STATIC` | Static 6-byte `node_id` override (string, optional) |
| `CONFIG_LIMA_SEQ_COUNTER_FLASH` | Persist sequence counter to flash (bool, default `y`) |

---

## 8. LED State Reference

For field diagnostics. Current as of `style(leds)` commit.

| State | LED Pattern | Notes |
|---|---|---|
| `BOOT` / `CALIBRATING` | Solid white | Init sequence |
| `ARMED` | Blue double-blink (2s period) | Heartbeat — node is active |
| `EVENT_DETECTED` / `SIGNING` / `TRANSMITTING` | Solid red | Alert pipeline active |
| `COOLDOWN` | Solid yellow (R+G) | Suppression window |
| `LIGHT_SLEEP` | Red+Blue slow pulse (2s) | Low-power, sensor IRQs active |
| `DEEP_SLEEP` | White ghost-pulse (4s) | BLE off, RTC wakeup only |
| `FAULT` / `LOW_BATTERY` | Solid red | Degraded operation |

---

## 9. Future Work

- Factory key provisioning via `nrfutil` KMU push
- Signed gateway commands for key rotation and remote wipe
- Payload encryption (AES-256-GCM) for BLE confidentiality
- Attestation token — include firmware version + boot measurement in payload
- `crypto_thread` — offload signing from FSM thread for high-throughput deployments
- Gateway trust store — public key registry and sequence counter tracking
- IEC 62443 alignment — traceability matrix between this design and SL-2 requirements

---

## 10. FSM Milestone Log

Key commits leading to crypto integration, in order. Every commit on `main` represents a green build with a verified `.cap` capture.

| Commit | Description |
|---|---|
| `feat(fsm): initial FSM skeleton` | BOOT/CALIBRATING/ARMED/FAULT states, msgq, sensor thread |
| `feat(sensors): MPU6050 + BME280 integration` | Live IMU + baro over I2C, motion threshold detection |
| `feat(fsm): sleep state infrastructure` | LIGHT_SLEEP, DEEP_SLEEP, LED blink patterns |
| `fix(fsm): armed dwell k_work_delayable` | Replaced immediate LIGHT_SLEEP transition with 3s dwell |
| `fix(fsm): remove blocking k_msleep from sleep HAL` | Non-blocking sleep — FSM thread stays live for events |
| `fix(fsm): LIGHT_SLEEP POLL_TICK handling` | Silence poll ticks in sleep states, inactivity timer owns wakeup |
| `style(leds): deep_sleep pulse to white ghost pattern` | LED scheme finalized, all states visually distinct |
| `feat(crypto): PSA ECDSA-P256 signing module` | **← YOU ARE HERE** |

---

*LIMA is an open-source project. Contributions welcome.*  
*github.com/jknoxdev/lima-node*
