/*
 * L.I.M.A. — Local Integrity Multi-modal Architecture
 * ble.c — BLE 5.0 extended non-connectable advertising of lima_lf_t (LIMA Frame)
 *
 * Encodes a pre-assembled lima_lf_t into manufacturer-specific AD data
 * and advertises as extended ADV_NONCONN_IND (184 bytes) via bt_le_ext_adv API.
 * Completion callback posts LIMA_EVT_TX_COMPLETE or LIMA_EVT_BLE_FAULT
 * to the FSM queue via fsm.c.
 *
 * Wire format spec: docs/dev/frame-record-spec.md
 *
 * Call order (from main.c and fsm.c):
 *   1. lima_ble_init()        — once, after bt_enable() in main.c
 *   2. lima_ble_advertise()   — called from state_transmitting_enter()
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <string.h>
#include "ble.h"
#include "crypto.h"

LOG_MODULE_REGISTER(lima_ble, CONFIG_LIMA_BLE_LOG_LEVEL);

/* ── Private state ───────────────────────────────────────────────────────── */

static bool                    ble_initialized = false;
static lima_ble_cb_t           adv_cb          = NULL;
static struct k_work_delayable adv_stop_work;
static struct bt_le_ext_adv   *ext_adv         = NULL;

/* ── AD buffers ──────────────────────────────────────────────────────────── */

/* LIMA Frame buffer — populated per-advertisement from the pre-assembled LF */
static lima_lf_t lf_buf;

static struct bt_data adv_data[] = {
    /* Manufacturer specific — 184 bytes: full LIMA Frame (LF) */
    BT_DATA(BT_DATA_MANUFACTURER_DATA,
            &lf_buf,
            sizeof(lima_lf_t)),
};

/* ── Advertisement stop work ─────────────────────────────────────────────── */

static void adv_stop_fn(struct k_work *work)
{
    ARG_UNUSED(work);

    int err = bt_le_ext_adv_stop(ext_adv);
    if (err) {
        LOG_ERR("BLE: ext adv stop failed (%d)", err);
        if (adv_cb) {
            adv_cb(LIMA_BLE_FAIL);
            adv_cb = NULL;
        }
        return;
    }

    LOG_INF("BLE: advertisement complete");
    if (adv_cb) {
        adv_cb(LIMA_BLE_OK);
        adv_cb = NULL;
    }
}

/* ── Public API ──────────────────────────────────────────────────────────── */

int lima_ble_init(void)
{
    if (ble_initialized) {
        LOG_WRN("BLE: already initialized");
        return 0;
    }

    k_work_init_delayable(&adv_stop_work, adv_stop_fn);

    /* Create extended advertising set — non-connectable, identity address */
    struct bt_le_adv_param param = BT_LE_ADV_PARAM_INIT(
        BT_LE_ADV_OPT_USE_IDENTITY |
        BT_LE_ADV_OPT_EXT_ADV,
        BT_GAP_ADV_FAST_INT_MIN_2,
        BT_GAP_ADV_FAST_INT_MAX_2,
        NULL
    );

    int err = bt_le_ext_adv_create(&param, NULL, &ext_adv);
    if (err) {
        LOG_ERR("BLE: ext adv create failed (%d)", err);
        return err;
    }

    ble_initialized = true;
    LOG_INF("BLE: initialized — LIMA node ready to advertise (ext ADV, 184B LF)");
    return 0;
}

int lima_ble_advertise(const lima_lf_t *lf, lima_ble_cb_t cb)
{
    if (!ble_initialized) {
        LOG_ERR("BLE: not initialized — call lima_ble_init() first");
        return -ECANCELED;
    }

    if (lf == NULL || cb == NULL) {
        LOG_ERR("BLE: NULL parameter");
        return -EINVAL;
    }

    /* Copy pre-assembled LF into the AD buffer */
    memcpy(&lf_buf, lf, sizeof(lima_lf_t));

    LOG_INF("BLE: advertising LF — proto=0x%02X evt=0x%02X "
            "nonce[0..3]=%02X%02X%02X%02X outer_sig[0..3]=%02X%02X%02X%02X",
            lf_buf.proto_version,
            lf_buf.event_type,
            lf_buf.nonce[0], lf_buf.nonce[1],
            lf_buf.nonce[2], lf_buf.nonce[3],
            lf_buf.outer_sig[0], lf_buf.outer_sig[1],
            lf_buf.outer_sig[2], lf_buf.outer_sig[3]);
    LOG_HEXDUMP_INF((const uint8_t *)&lf_buf, sizeof(lf_buf), "  LF:");

    /* Update advertising data */
    int err = bt_le_ext_adv_set_data(ext_adv,
                                     adv_data, ARRAY_SIZE(adv_data),
                                     NULL, 0);
    if (err) {
        LOG_ERR("BLE: ext adv set data failed (%d)", err);
        return err;
    }

    /* Store callback before starting */
    adv_cb = cb;

    /* Start extended advertising */
    struct bt_le_ext_adv_start_param start_param = {
        .timeout    = 0,   /* no timeout — stopped via work queue */
        .num_events = 0,   /* advertise indefinitely until stopped */
    };

    err = bt_le_ext_adv_start(ext_adv, &start_param);
    if (err) {
        LOG_ERR("BLE: ext adv start failed (%d)", err);
        adv_cb = NULL;
        return err;
    }

    LOG_INF("BLE: advertising for %d ms", CONFIG_LIMA_BLE_ADV_DURATION_MS);

    /* Schedule stop */
    k_work_reschedule(&adv_stop_work,
                      K_MSEC(CONFIG_LIMA_BLE_ADV_DURATION_MS));

    return 0;
}
