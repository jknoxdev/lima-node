/*
 * L.I.M.A. — Local Integrity Multi-modal Architecture
 * crypto.h — Signing and encryption API
 *
 * PSA ECDSA-P256 / SHA-256 inner signing over lima_ler_t.
 * PSA AES-256-GCM encryption of (LER || inner_sig) → lima_lf_t ciphertext.
 * PSA ECDSA-P256 / SHA-256 outer signing over LF[0..120].
 * All operations hardware-accelerated via CryptoCell-310.
 *
 * Key storage: Nordic KMU via persistent PSA key slots.
 *   CONFIG_LIMA_CRYPTO_KEY_ID     — ECDSA-P256 signing keypair
 *   CONFIG_LIMA_CRYPTO_AES_KEY_ID — AES-256 pre-shared key (PSK)
 *
 * Wire format spec: docs/architecture/frame-record-spec.md
 *
 * Call order (from main.c and fsm.c):
 *   1. lima_crypto_init()        — once, before fsm_init()
 *   2. lima_crypto_build_ler()   — populate LER from fsm.last_event
 *   3. lima_crypto_sign_async()  — inner sign LER → cb posts SIGNING_COMPLETE
 *   4. lima_crypto_build_lf()    — encrypt + outer sign → fully assembled LF
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
    uint8_t  node_id[6];
    uint8_t  event_type;
    uint8_t  reserved;
    uint32_t sequence;
    uint32_t timestamp_ms;
    float    accel_g;
    float    delta_pa;
} lima_ler_t;                 /* 24 bytes */

BUILD_ASSERT(sizeof(lima_ler_t) == 24, "lima_ler_t size mismatch");

/* ── LIMA Frame (LF) ─────────────────────────────────────────────────────── */
/*
 * Outer wire envelope — 184 bytes.
 * Encrypt-then-Sign: AES-256-GCM over (LER || inner_sig),
 * then ECDSA-P256 outer sig over LF[0..120].
 *
 * Offset  Size  Field           Notes
 *      0     1  proto_version   0x02
 *      1     1  event_type      mirrors LER.event_type — gateway pre-filter
 *      2     2  reserved        0x0000
 *      4    12  nonce           AES-256-GCM IV — random per frame
 *     16    88  ciphertext      AES-256-GCM encrypt(LER 24B || inner_sig 64B)
 *    104    16  gcm_tag         AES-256-GCM authentication tag
 *    120    64  outer_sig       ECDSA-P256 sig over LF[0..120]
 *           184 TOTAL
 */
typedef struct __attribute__((packed)) {
    uint8_t  proto_version;
    uint8_t  event_type;
    uint8_t  reserved[2];
    uint8_t  nonce[12];
    uint8_t  ciphertext[88];
    uint8_t  gcm_tag[16];
    uint8_t  outer_sig[64];
} lima_lf_t;                   /* 184 bytes */

BUILD_ASSERT(sizeof(lima_lf_t) == 184, "lima_lf_t size mismatch");

/* ── LF layout constants ─────────────────────────────────────────────────── */

#define LIMA_LF_HEADER_LEN      4    /* bytes covered by AAD (header only)    */
#define LIMA_LF_NONCE_LEN       12   /* AES-256-GCM IV                        */
#define LIMA_LF_PLAINTEXT_LEN   88   /* LER (24B) + inner_sig (64B)           */
#define LIMA_LF_CIPHERTEXT_LEN  88   /* GCM output same length as plaintext   */
#define LIMA_LF_TAG_LEN         16   /* AES-256-GCM auth tag                  */
#define LIMA_LF_OUTER_SIG_LEN   64   /* ECDSA-P256 outer sig                  */
#define LIMA_LF_SIGNED_LEN      120  /* bytes covered by outer_sig: LF[0..120]*/

/* ── Signing Result ──────────────────────────────────────────────────────── */

typedef struct {
    uint8_t  sig[64];
    size_t   sig_len;         /* always 64 for P-256 */
    int      err;             /* 0 = PSA_SUCCESS      */
} lima_sig_result_t;

/* ── Callback ────────────────────────────────────────────────────────────── */

typedef void (*lima_sign_cb_t)(const lima_sig_result_t *result);

/* ── API ─────────────────────────────────────────────────────────────────── */

/**
 * @brief Initialize PSA crypto and provision both keys if absent.
 *
 * Provisions on first boot:
 *   - ECDSA-P256 keypair  → CONFIG_LIMA_CRYPTO_KEY_ID
 *   - AES-256 PSK         → CONFIG_LIMA_CRYPTO_AES_KEY_ID
 *
 * AES key is generated on-device and logged in hex at INF level.
 * Operator captures from serial log and stores in Bitwarden secure note.
 * ECDSA public key is also logged for gateway registration.
 *
 * @return 0 on success, negative errno on failure.
 */
int lima_crypto_init(void);

/**
 * @brief Populate a lima_ler_t from the current FSM event.
 *
 * @param ler  Output struct to populate.
 * @param evt  Source event from fsm.last_event.
 */
void lima_crypto_build_ler(lima_ler_t *ler, const lima_event_t *evt);

/**
 * @brief Inner-sign a LER with ECDSA-P256 / SHA-256.
 *
 * CryptoCell hardware-accelerated. Calls cb() synchronously.
 * cb() posts LIMA_EVT_SIGNING_COMPLETE or LIMA_EVT_SENSOR_FAULT.
 *
 * @param ler  Populated LER struct.
 * @param cb   Completion callback — must be non-NULL.
 * @return 0 on success, negative on parameter error.
 */
int lima_crypto_sign_async(const lima_ler_t *ler, lima_sign_cb_t cb);

/**
 * @brief Assemble a fully encrypted and outer-signed lima_lf_t.
 *
 * Implements Encrypt-then-Sign (ADR-005). Three CryptoCell operations:
 *   1. psa_generate_random()   → nonce (12B)
 *   2. psa_aead_encrypt()      → AES-256-GCM(LER||inner_sig) → ciphertext+tag
 *   3. psa_sign_message()      → ECDSA-P256(LF[0..120]) → outer_sig
 *
 * Called from signing_complete_cb() in fsm.c after inner sign succeeds.
 *
 * @param ler           Populated LER (24B).
 * @param inner_sig     Inner ECDSA signature (64B).
 * @param inner_sig_len Must be 64.
 * @param out_lf        Output — fully assembled LF (184B).
 * @return 0 on success, negative errno on failure.
 */
int lima_crypto_build_lf(const lima_ler_t *ler,
                          const uint8_t *inner_sig,
                          size_t inner_sig_len,
                          lima_lf_t *out_lf);

#endif /* LIMA_CRYPTO_H */
