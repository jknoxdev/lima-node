/*
 * L.I.M.A. — Local Integrity Multi-modal Architecture
 * crypto.h — Signing API
 *
 * PSA ECDSA-P256 / SHA-256 over lima_ler_t (LIMA Event Record).
 * Key stored in Nordic KMU via persistent PSA key slot.
 *
 * Call order:
 *   1. lima_crypto_init()            — once, after psa_crypto_init()
 *   2. lima_crypto_build_ler()       — populate LER from fsm.last_event
 *   3. lima_crypto_sign_async()      — sign LER, callback posts SIGNING_COMPLETE
 *
 * Wire format spec: docs/dev/frame-record-spec.md
 */

#ifndef LIMA_CRYPTO_H
#define LIMA_CRYPTO_H

#include <stdint.h>
#include <stddef.h>
#include "events.h"

/* ── LIMA Event Record (LER) ─────────────────────────────────────────────── */
/*
 * Inner plaintext struct — 24 bytes.
 * Signed by node ECDSA-P256 key. Never transmitted in plaintext.
 * Always encrypted inside lima_lf_t before BLE transmission.
 *
 * Offset  Size  Field           Notes
 *      0     6  node_id         BLE MAC, big-endian
 *      6     1  event_type      lima_event_type_t
 *      7     1  reserved        Always 0x00 — alignment pad
 *      8     4  sequence        u32 LE — monotonic anti-replay counter
 *     12     4  timestamp_ms    u32 LE — RTC wall-clock epoch ms
 *     16     4  accel_g         f32 LE — IMU vector magnitude (g)
 *     20     4  delta_pa        f32 LE — barometric delta (Pa)
 *            24  TOTAL
 */
typedef struct __attribute__((packed)) {
    uint8_t  node_id[6];      /* BLE MAC or provisioned ID            */
    uint8_t  event_type;      /* lima_event_type_t                    */
    uint8_t  reserved;        /* alignment padding — zero-filled      */
    uint32_t sequence;        /* monotonic counter — anti-replay      */
    uint32_t timestamp_ms;    /* RTC wall-clock epoch ms              */
    float    accel_g;         /* IMU vector magnitude at trigger (g)  */
    float    delta_pa;        /* baro delta at trigger (Pa)           */
} lima_ler_t;                 /* 24 bytes                             */

BUILD_ASSERT(sizeof(lima_ler_t) == 24, "lima_ler_t size mismatch");

/* ── Signing Result ──────────────────────────────────────────────────────── */

typedef struct {
    uint8_t  sig[64];         /* ECDSA-P256 inner signature (r || s)  */
    size_t   sig_len;         /* always 64 for P-256                  */
    int      err;             /* 0 on success, PSA error code         */
} lima_sig_result_t;

/* ── Callback ────────────────────────────────────────────────────────────── */

typedef void (*lima_sign_cb_t)(const lima_sig_result_t *result);

/* ── API ─────────────────────────────────────────────────────────────────── */

/**
 * @brief Initialize PSA crypto and provision signing key if absent.
 *
 * Must be called once from main.c after board init, before fsm_init().
 * Generates a P-256 keypair into KMU slot CONFIG_LIMA_CRYPTO_KEY_ID
 * if no persistent key exists there yet.
 *
 * @return 0 on success, negative PSA error code on failure.
 */
int lima_crypto_init(void);

/**
 * @brief Populate a lima_ler_t (LIMA Event Record) from the current FSM event.
 *
 * Fills node_id from BLE MAC, stamps sequence counter, copies event fields.
 * Caller owns LER memory.
 *
 * @param ler  Output struct to populate.
 * @param evt  Source event from fsm.last_event.
 */
void lima_crypto_build_ler(lima_ler_t *ler, const lima_event_t *evt);

/**
 * @brief Sign a LER with ECDSA-P256 / SHA-256 (inner signature).
 *
 * Synchronous in v1 (CryptoCell hardware-accelerated but blocking).
 * Calls cb() with result before returning. cb() must post
 * LIMA_EVT_SIGNING_COMPLETE or LIMA_EVT_SENSOR_FAULT to the FSM queue.
 *
 * @param ler  Populated LER struct to sign.
 * @param cb   Completion callback — must be non-NULL.
 * @return 0 if signing was attempted, negative on parameter error.
 */
int lima_crypto_sign_async(const lima_ler_t *ler, lima_sign_cb_t cb);

#endif /* LIMA_CRYPTO_H */
