# LIMA: KMU / HSM Architecture Research Notes

**Date:** 2026-03-15  
**Status:** Research complete — documents current architecture, known limitations, and production hardening path  

---

## Summary

This document captures the findings from a deep-dive into key storage architecture on the nRF52840, clarifying what "KMU-backed key storage" actually means on this silicon, what is and isn't achievable without a silicon change, and how this affects LIMA's threat model.

---

## What the Code Actually Uses: PSA ITS (Not KMU Directly)

The `provision_key()` function uses `PSA_KEY_LIFETIME_PERSISTENT` which stores the ECDSA-P256 signing key through Zephyr's **PSA ITS (Internal Trusted Storage)** backend — a flash region managed by the Zephyr ITS shim. The key is encrypted at rest via ChaCha20-Poly1305 as configured by `CONFIG_SECURE_STORAGE`.

The KMU is involved, but one layer removed (see below).

---

## Key Storage Architecture: Actual Layering

```
KMU slot (hardware)
└── holds MKEK (Master Key Encryption Key)
        │
        ▼ ChaCha20-Poly1305 encryption/decryption
ITS (flash)
└── holds ECDSA-P256 signing key (ciphertext at rest)
        │
        ▼ decrypted transiently during signing
RAM
└── plaintext key material (only during CC310 signing operation)
```

- The **MKEK in KMU** is hardware-protected: software cannot read it, it can only be referenced. It never hits the CPU.
- The **ECDSA signing key** lives in ITS flash as ciphertext. During a signing operation it is decrypted into RAM, used by CC310, then discarded.
- The **plaintext signing key transiently exists in RAM** during signing operations. This is the residual exposure window.

---

## What KMU Actually Is

The nRF52840 KMU (Key Management Unit) is a hardware peripheral that holds keys in dedicated slot registers and can push them directly to CryptoCell (CC310) without ever exposing raw key bytes to the CPU. True KMU usage at the HAL level looks like `nrf_kmu_keyslot_push()`.

The current PSA/ITS path does **not** directly use KMU key slots for the signing key. It uses KMU only indirectly via the MKEK that protects ITS-stored keys.

---

## Why True KMU / TEE Is Not Achievable on nRF52840

### No TrustZone-M on This Silicon

The nRF52840 is a **Cortex-M4F** (ARMv7-M). TrustZone-M requires **ARMv8-M** (Cortex-M23 or M33). Therefore:

- No hardware Secure/Non-Secure world split
- No Trusted Execution Environment (TEE)
- TF-M (Trusted Firmware-M) is **not supported** on nRF52840

Nordic's TF-M support targets nRF5340 and nRF91 Series only.

### What a TEE Is

A TEE (Trusted Execution Environment) is a hardware-isolated execution environment that runs alongside the main OS/RTOS. Code and data inside the TEE is protected even if the main application is fully compromised. On ARM this is implemented via TrustZone. The Secure world is the TEE. TF-M is the reference software stack that runs inside it.

### CryptoCell RAM-Only Limitation

The CC310 core in the nRF52840 has **secure RAM but no secure flash**. It cannot hold a private key across power cycles on its own. This is why PSA ITS is the correct approach for this chip — the CryptoCell literally cannot do persistent key storage independently.

### Partition / TF-M Migration Would Require

If attempting TF-M on a supported chip (nRF5340/nRF91), the build would need to change from a flat single-image layout to:

```
Flash: [ MCUboot | TF-M SPE (Secure) | Application NSPE (Non-Secure) ]
RAM:   [ Secure RAM | Non-Secure RAM ]
SAU:   [ hardware boundary enforcement ]
```

This requires:
- A `pm_static.yml` partition manager config defining Secure/NS boundaries
- The app becomes an NSPE image calling into TF-M via PSA veneer layer
- TF-M is a separate firmware image built and flashed alongside the app
- Build system switches to Nordic's NCS west build with TF-M integration
- All existing BLE, crypto, and storage config needs re-validation

**This is weeks of work and a build system overhaul — not a config flag.**

### Known TF-M Limitations (Even on Supported Chips)

- Firmware Update service not supported
- AES-OFB and AES-CFB modes not supported
- Isolation Level 3 not supported
- In Isolation Level 2+, peripherals in ARoT limited by available MPU regions
- Only GCC toolchain supported for TF-M builds
- Secure/NS partition addresses must align to `CONFIG_NRF_SPU_FLASH_REGION_SIZE`

---

## OTA Reflash Attack Surface (nRF Connect / BLE DFU)

### What Exists

Nordic's **nRF Connect Device Manager** (iOS/Android) can push firmware OTA to any nRF52840 that exposes the DFU GATT service over BLE.

### Why LIMA Is Not Vulnerable to This

LIMA uses `ADV_NONCONN_IND` — **non-connectable advertising**. No BLE connection can be established. nRF Connect Device Manager cannot reach the device at all, regardless of whether a DFU service exists in firmware.

This was the correct design choice for a tamper-evident sensor and it closes the OTA reflash vector entirely.

### DFU Mode Entry Requirements (For Reference)

Even on a connectable device, DFU mode requires one of:
- No valid application in flash
- Physical button press
- Pin reset event
- GPREGRET register value set
- Buttonless DFU service explicitly enabled in app firmware

None of these are present in LIMA's current build.

---

## Threat Model Statement

> LIMA's signing key is encrypted at rest via a KMU-protected MKEK (ChaCha20-Poly1305). Key material is transiently exposed in RAM during signing operations due to the absence of TrustZone-M on Cortex-M4F silicon. Physical RAM extraction during an active signing window is the residual theoretical attack surface.
>
> Exploiting this requires: physical access to the node, SWD/JTAG debug port access or MCUboot bypass, deployment of malicious firmware with a DMA key extraction routine, and an exfiltration channel. Physical access to accomplish this is itself the event LIMA is designed to detect and attest via accelerometer tamper detection.
>
> The OTA reflash vector (nRF Connect Device Manager / BLE DFU) is closed by LIMA's non-connectable advertising design.
>
> **The architecture creates a self-defeating attack loop for the primary physical tamper threat model.**

---

## Production Hardening Path (Future Work)

| Option | Notes |
|---|---|
| **ATECC608B (discrete SE)** | I2C, Zephyr driver support exists, "key never leaves SE" is unambiguous, simplest path, no silicon change needed |
| **Migrate to nRF5340** | Dual-core, TF-M proper, KMU accessible from Secure core, significant board/build change |
| **Migrate to nRF9160** | TrustZone-M, good Nordic TF-M support, cellular-focused platform |
| **MCUboot + signed images** | Closes OTA and SWD reflash vectors, orthogonal to key storage, achievable on current silicon |

**Recommended near-term:** MCUboot with signed images (closes reflash vector, same silicon).  
**Recommended production path:** ATECC608B as discrete SE (clearest threat model story, no platform change).

---

## Log Message Correction

The current `provision_key()` log line should be updated for accuracy:

```c
// Current (misleading):
LOG_INF("CRYPTO: P-256 keypair generated (persistent, id=0x%08X)", *out_key_id);

// More accurate:
LOG_INF("CRYPTO: P-256 keypair generated (PSA ITS persistent, MKEK-protected, id=0x%08X)", *out_key_id);
```

---

*Research conducted 2026-03-15. Sources: Nordic DevZone, Zephyr docs, Nordic NCS TF-M documentation.*
