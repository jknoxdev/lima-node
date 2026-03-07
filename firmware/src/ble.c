/*
 * L.I.M.A. — Local Integrity Multi-modal Architecture
 * ble.c — BLE non-connectable advertising of signed lima_payload_t
 *
 * Encodes lima_payload_t into manufacturer-specific AD data and
 * advertises as ADV_NONCONN_IND for CONFIG_LIMA_BLE_ADV_DURATION_MS.
 * Completion callback posts LIMA_EVT_TX_COMPLETE or LIMA_EVT_BLE_FAULT
 * to the FSM queue via fsm.c.
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

static bool                ble_initialized = false;
static lima_ble_cb_t       adv_cb          = NULL;
static struct k_work_delayable adv_stop_work;

/* ── AD buffers ─────────────────────────────────────────────────────── */

/* Manufacturer-specific data buffer — populated per-advertisement */
static lima_adv_payload_t  adv_payload_buf;

static struct bt_data adv_data[] = {
    /* Flags: LE General Discoverable, BR/EDR not supported */
    BT_DATA_BYTES(BT_DATA_FLAGS,
                  BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
    /* Manufacturer specific — points into adv_payload_buf */
    BT_DATA(BT_DATA_MANUFACTURER_DATA,
            &adv_payload_buf,
            sizeof(lima_adv_payload_t)),
};

/* Scan response carries the device name */
static const struct bt_data sd[] = {
    BT_DATA(BT_DATA_NAME_COMPLETE,
            CONFIG_BT_DEVICE_NAME,
            sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

/* ── Advertisement stop work ─────────────────────────────────────────────── */

static void adv_stop_fn(struct k_work *work)
{
    ARG_UNUSED(work);

    int err = bt_le_adv_stop();
    if (err) {
        LOG_ERR("BLE: adv stop failed (%d)", err);
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

    ble_initialized = true;
    LOG_INF("BLE: initialized — LIMA_NODE_01 ready to advertise");
    return 0;
}

int lima_ble_advertise(const lima_payload_t *payload, lima_ble_cb_t cb)
{
    if (!ble_initialized) {
        LOG_ERR("BLE: not initialized — call lima_ble_init() first");
        return -ECANCELED;
    }

    if (payload == NULL || cb == NULL) {
        LOG_ERR("BLE: NULL parameter");
        return -EINVAL;
    }

    /* Encode lima_payload_t → lima_adv_payload_t */
    memset(&adv_payload_buf, 0, sizeof(adv_payload_buf));
    adv_payload_buf.company_id    = 0xFFFF;
    adv_payload_buf.proto_version = 0x01;
    adv_payload_buf.event_type    = payload->event_type;
    adv_payload_buf.sequence      = payload->sequence;
    adv_payload_buf.timestamp_ms  = payload->timestamp_ms;
    adv_payload_buf.accel_g       = payload->accel_g;
    adv_payload_buf.delta_pa      = payload->delta_pa;
    memcpy(adv_payload_buf.node_id, payload->node_id,
           sizeof(adv_payload_buf.node_id));

    LOG_INF("BLE: advertising — node=%02X:%02X:%02X:%02X:%02X:%02X "
            "evt=0x%02X seq=%u",
            adv_payload_buf.node_id[0], adv_payload_buf.node_id[1],
            adv_payload_buf.node_id[2], adv_payload_buf.node_id[3],
            adv_payload_buf.node_id[4], adv_payload_buf.node_id[5],
            adv_payload_buf.event_type,
            adv_payload_buf.sequence);

    /* Store callback before starting adv */
    adv_cb = cb;

    /* Non-connectable undirected advertisement */
    int err = bt_le_adv_start(
        BT_LE_ADV_PARAM(
            BT_LE_ADV_OPT_USE_IDENTITY,   /* use static identity address  */
            BT_GAP_ADV_FAST_INT_MIN_2,    /* 100ms min interval           */
            BT_GAP_ADV_FAST_INT_MAX_2,    /* 150ms max interval           */
            NULL                          /* non-connectable — no peer    */
        ),
        adv_data, ARRAY_SIZE(adv_data),
        sd,       ARRAY_SIZE(sd)
    );

    if (err) {
        LOG_ERR("BLE: adv start failed (%d)", err);
        adv_cb = NULL;
        return err;
    }

    LOG_INF("BLE: advertising for %d ms", CONFIG_LIMA_BLE_ADV_DURATION_MS);

    /* Schedule advertisement stop */
    k_work_reschedule(&adv_stop_work,
                      K_MSEC(CONFIG_LIMA_BLE_ADV_DURATION_MS));

    return 0;
}