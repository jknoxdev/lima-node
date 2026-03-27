/*
 * L.I.M.A. — Local Integrity Multi-modal Architecture
 * ble.h — BLE advertising API
 *
 * Non-connectable extended advertising of lima_lf_t (LIMA Frame).
 * Caller initializes once, then calls lima_ble_advertise() per event.
 * Completion callback posts LIMA_EVT_TX_COMPLETE or LIMA_EVT_BLE_FAULT
 * to the FSM queue.
 *
 * Call order:
 *   1. lima_ble_init()        — once, after bt_enable() in main.c
 *   2. lima_ble_advertise()   — called from state_transmitting_enter()
 *
 * Wire format spec: docs/dev/frame-record-spec.md
 */

#ifndef LIMA_BLE_H
#define LIMA_BLE_H

#include <stdint.h>
#include "crypto.h"

/* ── Result ──────────────────────────────────────────────────────────────── */

typedef enum {
    LIMA_BLE_OK   = 0,
    LIMA_BLE_FAIL = -1,
} lima_ble_err_t;

/* ── Callback ────────────────────────────────────────────────────────────── */

typedef void (*lima_ble_cb_t)(lima_ble_err_t err);

/* ── LIMA Frame (LF) ─────────────────────────────────────────────────────── */
/*
 * Outer wire envelope — 184 bytes.
 * Transmitted as BLE 5.0 extended advertising manufacturer-specific AD data.
 * Encrypt-then-Sign: AES-256-GCM over (LER || inner_sig), then
 * ECDSA-P256 outer signature over the full header+nonce+ciphertext.
 *
 * Offset  Size  Field           Notes
 *      0     1  proto_version   0x02
 *      1     1  event_type      Mirrors LER.event_type — gateway pre-filter
 *      2     2  reserved        0x0000
 *      4    12  nonce           AES-256-GCM IV — random per frame
 *     16    88  ciphertext      AES-256-GCM encrypt(LER 24B || inner_sig 64B)
 *    104    16  gcm_tag         AES-256-GCM authentication tag
 *    120    64  outer_sig       ECDSA-P256 sig over bytes[0..120]
 *           184 TOTAL
 *
 * Crypto layering:
 *   plaintext  = LER (24B) || inner_sig (64B)           = 88B
 *   ciphertext = AES-256-GCM-Encrypt(plaintext, nonce)  = 88B
 *   gcm_tag    = AES-256-GCM auth tag                   = 16B
 *   outer_sig  = ECDSA-P256-Sign(LF[0..120])            = 64B
 */
typedef struct __attribute__((packed)) {
    uint8_t  proto_version;    /* 0x02                                  */
    uint8_t  event_type;       /* mirrors LER.event_type                */
    uint8_t  reserved[2];      /* 0x0000                                */
    uint8_t  nonce[12];        /* AES-256-GCM IV — random per frame     */
    uint8_t  ciphertext[88];   /* AES-256-GCM encrypt(LER || inner_sig) */
    uint8_t  gcm_tag[16];      /* AES-256-GCM authentication tag        */
    uint8_t  outer_sig[64];    /* ECDSA-P256 sig over LF[0..120]        */
} lima_lf_t;                   /* 184 bytes                             */

BUILD_ASSERT(sizeof(lima_lf_t) == 184, "lima_lf_t size mismatch");

/* ── API ─────────────────────────────────────────────────────────────────── */

/**
 * @brief Initialize the BLE subsystem for LIMA advertising.
 *
 * Must be called once from main.c after bt_enable() completes.
 *
 * @return 0 on success, negative errno on failure.
 */
int lima_ble_init(void);

/**
 * @brief Advertise a LIMA Frame (LF) as a BLE 5.0 extended advertisement.
 *
 * Encodes LF into manufacturer-specific AD data and starts a
 * non-connectable extended advertisement. Advertisement stops
 * automatically after CONFIG_LIMA_BLE_ADV_DURATION_MS milliseconds.
 * cb() is invoked on completion or error.
 *
 * @param lf   Fully assembled LIMA Frame (encrypted + signed).
 * @param cb   Completion callback — must be non-NULL.
 * @return 0 if advertising started, negative errno on failure.
 */
int lima_ble_advertise(const lima_lf_t *lf, lima_ble_cb_t cb);

#endif /* LIMA_BLE_H */
