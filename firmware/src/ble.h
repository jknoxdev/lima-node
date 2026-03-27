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
