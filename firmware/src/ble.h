/*
 * L.I.M.A. — Local Integrity Multi-modal Architecture
 * ble.h — BLE advertising API
 *
 * Non-connectable undirected advertising of signed lima_payload_t.
 * Caller initializes once, then calls lima_ble_advertise() per event.
 * Completion callback posts LIMA_EVT_TX_COMPLETE or LIMA_EVT_BLE_FAULT
 * to the FSM queue.
 *
 * Call order:
 *   1. lima_ble_init()        — once, after bt_enable() in main.c
 *   2. lima_ble_advertise()   — called from state_transmitting_enter()
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

/* ── Advertised packet ───────────────────────────────────────────────────── */

/*
 * Wire format — manufacturer-specific AD data (26 bytes):
 *
 *  Offset  Size  Field
 *  ------  ----  -----
 *       0     2  Company ID (0xFFFF — test/internal use)
 *       2     1  LIMA protocol version (0x01)
 *       3     1  event_type
 *       4     4  sequence (little-endian)
 *       5     4  timestamp_ms (little-endian)  [offset 8 from AD start]
 *      12     4  accel_g (IEEE 754 float, little-endian)
 *      16     4  delta_pa (IEEE 754 float, little-endian)
 *      20     6  node_id
 *            26  total
 *
 * Signature is NOT included in v1 — 64-byte ECDSA sig exceeds 31-byte
 * AD limit. Signing occurs on-device; public key registered at gateway
 * provisioning. See ADR-007.
 */
typedef struct __attribute__((packed)) {
    uint16_t company_id;      /* 0xFFFF — internal/test                */
    uint8_t  proto_version;   /* 0x01                                  */
    uint8_t  event_type;
    uint32_t sequence;
    uint32_t timestamp_ms;
    float    accel_g;
    float    delta_pa;
    uint8_t  node_id[6];
} lima_adv_payload_t;         /* 26 bytes                              */

BUILD_ASSERT(sizeof(lima_adv_payload_t) == 26, "lima_adv_payload_t size mismatch");

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
 * @brief Advertise a signed payload as a non-connectable BLE advertisement.
 *
 * Encodes payload into lima_adv_payload_t manufacturer-specific AD data
 * and starts a non-connectable undirected advertisement. Advertisement
 * stops automatically after CONFIG_LIMA_BLE_ADV_DURATION_MS milliseconds.
 * cb() is invoked on completion or error.
 *
 * @param payload   Populated payload struct from lima_crypto_build_payload().
 * @param cb        Completion callback — must be non-NULL.
 * @return 0 if advertising started, negative errno on failure.
 */
int lima_ble_advertise(const lima_payload_t *payload, lima_ble_cb_t cb);

#endif /* LIMA_BLE_H */