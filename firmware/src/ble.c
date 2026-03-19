 /*
 * L.I.M.A. — Local Integrity Multi-modal Architecture
 * ble.c — BLE 5.0 extended non-connectable advertising of signed lima_payload_t
 *
 * Encodes lima_payload_t + ECDSA-P256 sig into manufacturer-specific AD data
 * and advertises as extended ADV_NONCONN_IND (90 bytes) via bt_le_ext_adv API.
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

static bool                    ble_initialized = false;
static lima_ble_cb_t           adv_cb          = NULL;
static struct k_work_delayable adv_stop_work;
static struct bt_le_ext_adv   *ext_adv         = NULL;

/* ── AD buffers ─────────────────────────────────────────────────────── */

/* Manufacturer-specific data buffer — populated per-advertisement */
static lima_adv_payload_t  adv_payload_buf;

static struct bt_data adv_data[] = {
    /* Manufacturer specific — 90 bytes: payload + sig */
    /* NOTE: no BT_DATA_FLAGS for extended non-connectable ADV */
    BT_DATA(BT_DATA_MANUFACTURER_DATA,
            &adv_payload_buf,
            sizeof(lima_adv_payload_t)),
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
    LOG_INF("BLE: initialized — LIMA_NODE_01 ready to advertise (ext ADV)");
    return 0;
}

int lima_ble_advertise(const lima_payload_t *payload,
                       const uint8_t *sig,
                       size_t sig_len,
                       lima_ble_cb_t cb)
{
    if (!ble_initialized) {
        LOG_ERR("BLE: not initialized — call lima_ble_init() first");
        return -ECANCELED;
    }

    if (payload == NULL || sig == NULL || cb == NULL) {
        LOG_ERR("BLE: NULL parameter");
        return -EINVAL;
    }

    /* Encode lima_payload_t + sig → lima_adv_payload_t */
    memset(&adv_payload_buf, 0, sizeof(adv_payload_buf));
    adv_payload_buf.company_id    = 0xFFFF;
    adv_payload_buf.proto_version = 0x02;
    adv_payload_buf.event_type    = payload->event_type;
    adv_payload_buf.sequence      = payload->sequence;
    adv_payload_buf.timestamp_ms  = payload->timestamp_ms;
    adv_payload_buf.accel_g       = payload->accel_g;
    adv_payload_buf.delta_pa      = payload->delta_pa;
    memcpy(adv_payload_buf.node_id, payload->node_id,
           sizeof(adv_payload_buf.node_id));
    memcpy(adv_payload_buf.sig, sig,
           MIN(sig_len, sizeof(adv_payload_buf.sig)));

    LOG_INF("BLE: advertising — node=%02X:%02X:%02X:%02X:%02X:%02X "
            "evt=0x%02X seq=%u accel=%.2f delta_pa=%.2f sig[0..3]=%02X%02X%02X%02X",
            adv_payload_buf.node_id[0], adv_payload_buf.node_id[1],
            adv_payload_buf.node_id[2], adv_payload_buf.node_id[3],
            adv_payload_buf.node_id[4], adv_payload_buf.node_id[5],
            adv_payload_buf.event_type,
            adv_payload_buf.sequence,
            (double)adv_payload_buf.accel_g,
            (double)adv_payload_buf.delta_pa,
            adv_payload_buf.sig[0], adv_payload_buf.sig[1],
            adv_payload_buf.sig[2], adv_payload_buf.sig[3]);
    LOG_HEXDUMP_INF((const uint8_t *)&adv_payload_buf,
                    sizeof(adv_payload_buf), "  adv_payload:");

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