/*
 * L.I.M.A. — Local Integrity Multi-modal Architecture
 * fsm.c — State machine logic
 *
 * This file owns all state transition logic. It communicates with
 * hardware exclusively through the fsm_hw_* hooks implemented in main.c.
 * It posts events back to the queue via lima_post_event() declared in fsm.h.
 *
 * Wire format spec: docs/dev/frame-record-spec.md
 */

#include <zephyr/logging/log.h>
#include <psa/crypto.h>
#include "fsm.h"
#include "crypto.h"
#include "ble.h"
#include "rtc.h"

LOG_MODULE_REGISTER(lima_fsm, LOG_LEVEL_INF);

/* ── FSM Context (single instance) ──────────────────────────────────────── */
lima_fsm_ctx_t fsm = {
    .cooldown_ms   = 5000,
    .fault_retries = 0,
};

/* ── Internal State (Private to this file) ──────────────────────────────── */
static lima_state_t current_state = STATE_BOOT;

/* timeouts and their handlers */
static struct k_work_delayable cooldown_work;
static struct k_work_delayable tx_timeout_work;
static struct k_work_delayable armed_dwell_work;
static struct k_work_delayable inactivity_work;

/* ── Forward Declarations ────────────────────────────────────────────────── */

static void transition(lima_state_t next);

/* Work/timer callbacks */
static void cooldown_expiry_cb(struct k_work *work);
static void tx_timeout_cb(struct k_work *work);
static void armed_dwell_expiry_cb(struct k_work *work);
static void inactivity_expiry_cb(struct k_work *work);

/* State entry functions */
static void state_boot_enter(void);
static void state_calibrating_enter(void);
static void state_armed_enter(void);
static void state_armed_exit(void);
static void state_light_sleep_enter(void);
static void state_deep_sleep_enter(void);
static void state_event_detected_enter(void);
static void state_signing_enter(void);
static void state_transmitting_enter(void);
static void state_cooldown_enter(void);
static void state_fault_enter(void);
static void state_low_battery_enter(void);

/* State event handlers */
static void state_armed_handle(const lima_event_t *evt);
static void state_light_sleep_handle(const lima_event_t *evt);
static void state_deep_sleep_handle(const lima_event_t *evt);
static void state_signing_handle(const lima_event_t *evt);
static void state_transmitting_handle(const lima_event_t *evt);
static void state_cooldown_handle(const lima_event_t *evt);
static void state_fault_handle(const lima_event_t *evt);
static void state_low_battery_handle(const lima_event_t *evt);

/* ── State Name Table ────────────────────────────────────────────────────── */

const char *fsm_state_to_str(lima_state_t state)
{
    static const char *names[STATE_COUNT] = {
        [STATE_BOOT]           = "BOOT",
        [STATE_CALIBRATING]    = "CALIBRATING",
        [STATE_ARMED]          = "ARMED",
        [STATE_LIGHT_SLEEP]    = "LIGHT_SLEEP",
        [STATE_DEEP_SLEEP]     = "DEEP_SLEEP",
        [STATE_EVENT_DETECTED] = "EVENT_DETECTED",
        [STATE_SIGNING]        = "SIGNING",
        [STATE_TRANSMITTING]   = "TRANSMITTING",
        [STATE_COOLDOWN]       = "COOLDOWN",
        [STATE_FAULT]          = "FAULT",
        [STATE_LOW_BATTERY]    = "LOW_BATTERY",
    };

    if (state >= STATE_COUNT) {
        return "UNKNOWN";
    }
    return names[state];
}

/* ── Transition Engine ───────────────────────────────────────────────────── */

static void transition(lima_state_t next)
{
    LOG_INF("FSM: %s -> %s",
            fsm_state_to_str(current_state),
            fsm_state_to_str(next));

    /* EXIT actions for the current state */
    switch (current_state) {
    case STATE_ARMED:
        state_armed_exit();
        break;
    case STATE_COOLDOWN:
        k_work_cancel_delayable(&cooldown_work);
        break;
    case STATE_TRANSMITTING:
        k_work_cancel_delayable(&tx_timeout_work);
        break;
    default:
        break;
    }

    current_state = next;
    fsm_hw_set_led(current_state);

    /* ENTRY actions for the new state */
    switch (current_state) {
    case STATE_BOOT:           state_boot_enter();           break;
    case STATE_CALIBRATING:    state_calibrating_enter();    break;
    case STATE_ARMED:          state_armed_enter();          break;
    case STATE_LIGHT_SLEEP:    state_light_sleep_enter();    break;
    case STATE_DEEP_SLEEP:     state_deep_sleep_enter();     break;
    case STATE_EVENT_DETECTED: state_event_detected_enter(); break;
    case STATE_SIGNING:        state_signing_enter();        break;
    case STATE_TRANSMITTING:   state_transmitting_enter();   break;
    case STATE_COOLDOWN:       state_cooldown_enter();       break;
    case STATE_FAULT:          state_fault_enter();          break;
    case STATE_LOW_BATTERY:    state_low_battery_enter();    break;
    default:                                                 break;
    }
}

/* ── Work Queue Callbacks ────────────────────────────────────────────────── */

static void cooldown_expiry_cb(struct k_work *work)
{
    ARG_UNUSED(work);
    lima_event_t e = {
        .type         = LIMA_EVT_COOLDOWN_EXPIRED,
        .timestamp_ms = k_uptime_get_32(),
    };
    LOG_INF("COOLDOWN: timer expired");
    lima_post_event(&e);
}

static void tx_timeout_cb(struct k_work *work)
{
    ARG_UNUSED(work);
    lima_event_t e = {
        .type         = LIMA_EVT_TX_TIMEOUT,
        .timestamp_ms = k_uptime_get_32(),
    };
    LOG_WRN("TRANSMITTING: timeout -> forcing COOLDOWN");
    lima_post_event(&e);
}

static void armed_dwell_expiry_cb(struct k_work *work)
{
    ARG_UNUSED(work);
    lima_event_t e = {
        .type         = LIMA_EVT_ARMED_TIMEOUT,
        .timestamp_ms = k_uptime_get_32(),
    };
    LOG_INF("ARMED: dwell timer expired -> sleep eligible");
    lima_post_event(&e);
}

static void inactivity_expiry_cb(struct k_work *work)
{
    ARG_UNUSED(work);
    lima_event_t e = {
        .type         = LIMA_EVT_INACTIVITY_TIMEOUT,
        .timestamp_ms = k_uptime_get_32(),
    };
    LOG_INF("INACTIVITY: 30s window reached -> Deep Sleep");
    lima_post_event(&e);
}

/* ── State: BOOT ─────────────────────────────────────────────────────────── */

static void state_boot_enter(void)
{
    LOG_INF("BOOT: entered");
    lima_event_t e = {
        .type         = LIMA_EVT_INIT_COMPLETE,
        .timestamp_ms = k_uptime_get_32(),
    };
    lima_post_event(&e);
}

/* ── State: CALIBRATING ──────────────────────────────────────────────────── */

static void state_calibrating_enter(void)
{
    LOG_INF("CALIBRATING: warming up sensors");
    lima_event_t e = {
        .type         = LIMA_EVT_BASELINE_READY,
        .timestamp_ms = k_uptime_get_32(),
    };
    lima_post_event(&e);
}

/* ── State: ARMED ────────────────────────────────────────────────────────── */

static void state_armed_enter(void)
{
    LOG_INF("ARMED: sensors active, heartbeat started");
    fsm.armed_since_ms = k_uptime_get_32();

    k_work_reschedule(&inactivity_work, K_MSEC(SLEEP_INACTIVITY_MS));
    k_work_reschedule(&armed_dwell_work, K_MSEC(ARMED_DWELL_MS));
}

static void state_armed_exit(void)
{
    LOG_DBG("ARMED: exit — cancelling dwell and inactivity timers");
    k_work_cancel_delayable(&armed_dwell_work);
}

static void state_armed_handle(const lima_event_t *evt)
{
    switch (evt->type) {
        case LIMA_EVT_PRESSURE_BREACH:
        case LIMA_EVT_MOTION_DETECTED:
        case LIMA_EVT_DUAL_BREACH:
        case LIMA_EVT_TAMPER_DETECTED:
            k_work_reschedule(&inactivity_work, K_MSEC(SLEEP_INACTIVITY_MS));
            fsm.last_event = *evt;
            transition(STATE_EVENT_DETECTED);
            break;

        case LIMA_EVT_INACTIVITY_TIMEOUT:
            transition(STATE_DEEP_SLEEP);
            break;

        case LIMA_EVT_ARMED_TIMEOUT:
            LOG_INF("ARMED: dwell complete -> LIGHT_SLEEP");
            transition(STATE_LIGHT_SLEEP);
            break;

        case LIMA_EVT_POLL_TICK:
            LOG_DBG("ARMED: tick (dwell active)");
            break;

        case LIMA_EVT_LOW_BATTERY:
        case LIMA_EVT_CRITICAL_BATTERY:
            transition(STATE_LOW_BATTERY);
            break;

        case LIMA_EVT_SENSOR_FAULT:
            transition(STATE_FAULT);
            break;

        default:
            LOG_WRN("ARMED: unhandled event type=0x%02X", evt->type);
            break;
    }
}

/* ── State: LIGHT_SLEEP ──────────────────────────────────────────────────── */

static void state_light_sleep_enter(void)
{
    LOG_INF("LIGHT SLEEP: low-power, sensor IRQs active");
    fsm_hw_enter_sleep();
}

static void state_light_sleep_handle(const lima_event_t *evt)
{
    switch (evt->type) {
    case LIMA_EVT_PRESSURE_BREACH:
    case LIMA_EVT_MOTION_DETECTED:
    case LIMA_EVT_DUAL_BREACH:
    case LIMA_EVT_TAMPER_DETECTED:
        fsm.last_event = *evt;
        transition(STATE_EVENT_DETECTED);
        break;

    case LIMA_EVT_POLL_TICK:
        LOG_DBG("LIGHT_SLEEP: ignoring poll tick");
        break;

    case LIMA_EVT_INACTIVITY_TIMEOUT:
        transition(STATE_DEEP_SLEEP);
        break;

    case LIMA_EVT_LOW_BATTERY:
    case LIMA_EVT_CRITICAL_BATTERY:
        transition(STATE_LOW_BATTERY);
        break;

    default:
        LOG_WRN("LIGHT_SLEEP: unhandled event 0x%02X", evt->type);
        break;
    }
}

/* ── State: DEEP_SLEEP ───────────────────────────────────────────────────── */

static void state_deep_sleep_enter(void)
{
    LOG_INF("DEEP SLEEP: BLE off, RTC wakeup only");
    fsm_hw_enter_deep_sleep();
}

static void state_deep_sleep_handle(const lima_event_t *evt)
{
    switch (evt->type) {
    case LIMA_EVT_RTC_WAKEUP:
        LOG_INF("DEEP SLEEP: RTC wakeup -> ARMED");
        transition(STATE_ARMED);
        break;

    case LIMA_EVT_LOW_BATTERY:
    case LIMA_EVT_CRITICAL_BATTERY:
        transition(STATE_LOW_BATTERY);
        break;

    case LIMA_EVT_POLL_TICK:
        break;

    default:
        LOG_WRN("DEEP_SLEEP: unhandled event 0x%02X", evt->type);
        break;
    }
}

/* ── State: EVENT_DETECTED ───────────────────────────────────────────────── */

static void state_event_detected_enter(void)
{
    char tsbuf[32];
    lima_rtc_format_now(tsbuf, sizeof(tsbuf));
    LOG_INF("EVENT DETECTED: type=0x%02X at t=%u ms | %s",
            fsm.last_event.type,
            fsm.last_event.timestamp_ms,
            tsbuf);

    transition(STATE_SIGNING);
}

/* ── State: SIGNING ──────────────────────────────────────────────────────── */

/*
 * signing_complete_cb — called by lima_crypto_sign_async() with inner_sig result.
 *
 * Assembles lima_lf_t (LIMA Frame) from LER + inner_sig.
 *
 * STUB (feat/frame-record-spec): AES-256-GCM encryption not yet applied.
 * ciphertext field = LER || inner_sig in plaintext.
 * nonce and gcm_tag are zeroed.
 * outer_sig = inner_sig (placeholder — proper outer sig over LF[0..120] is
 *             implemented in feat/ler-encrypt).
 *
 * TODO(feat/ler-encrypt): replace stub body with:
 *   1. psa_generate_random(nonce, 12)
 *   2. psa_aead_encrypt(AES-256-GCM, plaintext → ciphertext + gcm_tag)
 *   3. psa_sign_message(ECDSA-P256, LF[0..120] → outer_sig)
 */
static void signing_complete_cb(const lima_sig_result_t *result)
{
    if (result->err != PSA_SUCCESS) {
        LOG_ERR("SIGNING: crypto failed (%d) -> FAULT", result->err);
        lima_event_t e = {
            .type         = LIMA_EVT_SENSOR_FAULT,
            .timestamp_ms = k_uptime_get_32(),
        };
        lima_post_event(&e);
        return;
    }

    /* ── Assemble LIMA Frame (LF) ──────────────────────────────────────── */
    memset(&fsm.last_lf, 0, sizeof(fsm.last_lf));

    /* Header */
    fsm.last_lf.proto_version = 0x02;
    fsm.last_lf.event_type    = fsm.last_ler.event_type;
    /* reserved stays zero */

    /* STUB: nonce = zeros (no AES-GCM yet) */
    memset(fsm.last_lf.nonce, 0, sizeof(fsm.last_lf.nonce));

    /* STUB: ciphertext = LER (24B) || inner_sig (64B) in plaintext */
    BUILD_ASSERT(sizeof(fsm.last_ler) + 64 == sizeof(fsm.last_lf.ciphertext),
                 "LF ciphertext size must equal LER + inner_sig");
    memcpy(fsm.last_lf.ciphertext,
           &fsm.last_ler,
           sizeof(fsm.last_ler));
    memcpy(fsm.last_lf.ciphertext + sizeof(fsm.last_ler),
           result->sig,
           result->sig_len);

    /* STUB: gcm_tag = zeros (no AES-GCM yet) */
    memset(fsm.last_lf.gcm_tag, 0, sizeof(fsm.last_lf.gcm_tag));

    /* STUB: outer_sig = inner_sig (placeholder — real outer sig covers LF[0..120]) */
    memcpy(fsm.last_lf.outer_sig, result->sig, result->sig_len);

    LOG_INF("SIGNING: LF assembled — proto=0x%02X evt=0x%02X "
            "inner_sig[0..3]=%02X%02X%02X%02X [STUB: no AES-GCM]",
            fsm.last_lf.proto_version,
            fsm.last_lf.event_type,
            result->sig[0], result->sig[1],
            result->sig[2], result->sig[3]);
    LOG_HEXDUMP_INF((const uint8_t *)&fsm.last_lf, sizeof(fsm.last_lf), "  LF:");

    lima_event_t e = {
        .type         = LIMA_EVT_SIGNING_COMPLETE,
        .timestamp_ms = k_uptime_get_32(),
    };
    lima_post_event(&e);
}

static void state_signing_enter(void)
{
    LOG_INF("SIGNING: building LER and signing...");

    lima_crypto_build_ler(&fsm.last_ler, &fsm.last_event);

    int rc = lima_crypto_sign_async(&fsm.last_ler, signing_complete_cb);
    if (rc != 0) {
        LOG_ERR("SIGNING: failed to start signing (%d) -> FAULT", rc);
        transition(STATE_FAULT);
    }
}

static void state_signing_handle(const lima_event_t *evt)
{
    switch (evt->type) {
    case LIMA_EVT_SIGNING_COMPLETE:
        LOG_INF("SIGNING: LF ready -> TRANSMITTING");
        transition(STATE_TRANSMITTING);
        break;

    case LIMA_EVT_SENSOR_FAULT:
        LOG_WRN("SIGNING: fault during signing -> FAULT");
        transition(STATE_FAULT);
        break;

    default:
        LOG_WRN("SIGNING: unhandled event 0x%02X", evt->type);
        break;
    }
}

/* ── State: TRANSMITTING ─────────────────────────────────────────────────── */

static void ble_tx_complete_cb(lima_ble_err_t err)
{
    lima_event_t e = {
        .timestamp_ms = k_uptime_get_32(),
    };

    if (err == LIMA_BLE_OK) {
        e.type = LIMA_EVT_TX_COMPLETE;
        LOG_INF("TRANSMITTING: BLE advertisement complete");
    } else {
        e.type = LIMA_EVT_BLE_FAULT;
        LOG_ERR("TRANSMITTING: BLE advertisement failed");
    }

    lima_post_event(&e);
}

static void state_transmitting_enter(void)
{
    LOG_INF("TRANSMITTING: advertising LIMA Frame (LF) via BLE");

    k_work_reschedule(&tx_timeout_work, K_MSEC(TX_TIMEOUT_MS));

    int err = lima_ble_advertise(&fsm.last_lf, ble_tx_complete_cb);
    if (err != 0) {
        LOG_ERR("TRANSMITTING: failed to start BLE adv (%d) -> FAULT", err);
        transition(STATE_FAULT);
    }
}

static void state_transmitting_handle(const lima_event_t *evt)
{
    switch (evt->type) {
    case LIMA_EVT_TX_COMPLETE:
        k_work_cancel_delayable(&tx_timeout_work);
        LOG_INF("TRANSMITTING: confirmed -> COOLDOWN");
        transition(STATE_COOLDOWN);
        break;

    case LIMA_EVT_TX_TIMEOUT:
        LOG_WRN("TRANSMITTING: timeout -> COOLDOWN");
        transition(STATE_COOLDOWN);
        break;

    case LIMA_EVT_BLE_FAULT:
        fsm.fault_retries = 0;
        transition(STATE_FAULT);
        break;

    default:
        LOG_WRN("TRANSMITTING: unhandled event 0x%02X", evt->type);
        break;
    }
}

/* ── State: COOLDOWN ─────────────────────────────────────────────────────── */

static void state_cooldown_enter(void)
{
    uint32_t ms = fsm.cooldown_ms > 0 ? fsm.cooldown_ms : 5000;
    LOG_INF("COOLDOWN: suppressing events for %u ms", ms);
    k_work_reschedule(&cooldown_work, K_MSEC(ms));
}

static void state_cooldown_handle(const lima_event_t *evt)
{
    switch (evt->type) {
    case LIMA_EVT_COOLDOWN_EXPIRED:
        LOG_INF("COOLDOWN: expired -> ARMED");
        transition(STATE_ARMED);
        break;

    case LIMA_EVT_TAMPER_DETECTED:
        LOG_WRN("COOLDOWN: tamper override! Aborting cooldown.");
        k_work_cancel_delayable(&cooldown_work);
        fsm.last_event = *evt;
        transition(STATE_EVENT_DETECTED);
        break;

    case LIMA_EVT_LOW_BATTERY:
    case LIMA_EVT_CRITICAL_BATTERY:
        LOG_WRN("COOLDOWN: battery event override.");
        k_work_cancel_delayable(&cooldown_work);
        transition(STATE_LOW_BATTERY);
        break;

    default:
        LOG_DBG("COOLDOWN: suppressed event 0x%02X", evt->type);
        break;
    }
}

/* ── State: FAULT ────────────────────────────────────────────────────────── */

static void state_fault_enter(void)
{
    LOG_ERR("FAULT: entered (retry %d/%d)", fsm.fault_retries, MAX_FAULT_RETRIES);

    if (fsm.fault_retries >= MAX_FAULT_RETRIES) {
        LOG_ERR("FAULT: max retries exceeded -> requesting watchdog reset");
        lima_event_t e = {
            .type         = LIMA_EVT_ERROR,
            .timestamp_ms = k_uptime_get_32(),
        };
        lima_post_event(&e);
        return;
    }

    fsm.fault_retries++;

    lima_event_t e = {
        .type         = LIMA_EVT_RECOVERY_SUCCESS,
        .timestamp_ms = k_uptime_get_32(),
    };
    lima_post_event(&e);
}

static void state_fault_handle(const lima_event_t *evt)
{
    switch (evt->type) {
    case LIMA_EVT_RECOVERY_SUCCESS:
        LOG_INF("FAULT: recovery succeeded -> CALIBRATING");
        fsm.fault_retries = 0;
        transition(STATE_CALIBRATING);
        break;

    case LIMA_EVT_RECOVERY_FAILED:
    case LIMA_EVT_ERROR:
        LOG_ERR("FAULT: unrecoverable — watchdog reset required");
        break;

    default:
        LOG_WRN("FAULT: unhandled event 0x%02X", evt->type);
        break;
    }
}

/* ── State: LOW_BATTERY ──────────────────────────────────────────────────── */

static void state_low_battery_enter(void)
{
    LOG_WRN("LOW BATTERY: reducing poll rate, notifying gateway");
}

static void state_low_battery_handle(const lima_event_t *evt)
{
    switch (evt->type) {
    case LIMA_EVT_BATTERY_RESTORED:
        LOG_INF("LOW BATTERY: Vbat restored -> ARMED");
        transition(STATE_ARMED);
        break;

    case LIMA_EVT_CRITICAL_BATTERY:
        LOG_ERR("LOW BATTERY: critical Vbat -> shutdown");
        break;

    case LIMA_EVT_PRESSURE_BREACH:
    case LIMA_EVT_MOTION_DETECTED:
    case LIMA_EVT_DUAL_BREACH:
    case LIMA_EVT_TAMPER_DETECTED:
        fsm.last_event = *evt;
        transition(STATE_EVENT_DETECTED);
        break;

    default:
        LOG_WRN("LOW_BATTERY: unhandled event 0x%02X", evt->type);
        break;
    }
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void fsm_init(void)
{
    LOG_INF("FSM: Initializing work items");

    k_work_init_delayable(&cooldown_work, cooldown_expiry_cb);
    k_work_init_delayable(&tx_timeout_work, tx_timeout_cb);
    k_work_init_delayable(&armed_dwell_work, armed_dwell_expiry_cb);
    k_work_init_delayable(&inactivity_work, inactivity_expiry_cb);

    current_state = STATE_BOOT;
    fsm_hw_set_led(STATE_BOOT);
    state_boot_enter();

    LOG_INF("FSM: Initialized in %s", fsm_state_to_str(current_state));
}

lima_state_t fsm_get_state(void)
{
    return current_state;
}

void fsm_dispatch(const lima_event_t *evt)
{
    fsm_hw_wdt_kick();

    /* Lifecycle events that drive boot sequencing */
    if (current_state == STATE_BOOT && evt->type == LIMA_EVT_INIT_COMPLETE) {
        transition(STATE_CALIBRATING);
        return;
    }
    if (current_state == STATE_CALIBRATING && evt->type == LIMA_EVT_BASELINE_READY) {
        transition(STATE_ARMED);
        return;
    }

    /* Route event to the active state's handler */
    switch (current_state) {
    case STATE_ARMED:          state_armed_handle(evt);          break;
    case STATE_LIGHT_SLEEP:    state_light_sleep_handle(evt);    break;
    case STATE_DEEP_SLEEP:     state_deep_sleep_handle(evt);     break;
    case STATE_SIGNING:        state_signing_handle(evt);        break;
    case STATE_TRANSMITTING:   state_transmitting_handle(evt);   break;
    case STATE_COOLDOWN:       state_cooldown_handle(evt);       break;
    case STATE_FAULT:          state_fault_handle(evt);          break;
    case STATE_LOW_BATTERY:    state_low_battery_handle(evt);    break;

    /* Transient entry-only states — no external events handled */
    case STATE_BOOT:
    case STATE_CALIBRATING:
    case STATE_EVENT_DETECTED:
        LOG_WRN("FSM: event 0x%02X in transient state %s — ignored",
                evt->type, fsm_state_to_str(current_state));
        break;

    default:
        LOG_ERR("FSM: dispatch in unknown state %d", current_state);
        break;
    }
}
