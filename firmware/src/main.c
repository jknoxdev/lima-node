/*
 * L.I.M.A. — Local Integrity Multi-modal Architecture
 * main.c — Firmware State Machine with MPU6050 sensor integration
 *
 * Implements every state from docs/architecture/state.puml.
 * MPU6050 accel/gyro is live. BMP280, CryptoCell, and BLE are
 * still stubbed — each gets swapped in one at a time.
 *
 * Thread model:
 *   - fsm_thread    : owns all state transitions (this file)
 *   - sensor_thread : polls IMU, posts events to fsm_msgq
 *
 * Event flow:
 *   sensor_thread reads MPU6050
 *       → lima_event_t posted to fsm_msgq
 *           → fsm_thread wakes, calls state handler
 *               → transitions to next state
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>
#include <math.h>

#include "events.h"

LOG_MODULE_REGISTER(lima_fsm, LOG_LEVEL_DBG);

/* ── Board definitions ───────────────────────────────────────────────────── */

#define LED0_NODE DT_ALIAS(led0)

/* ── Configuration ───────────────────────────────────────────────────────── */

#define FSM_MSGQ_DEPTH          16
#define FSM_STACK_SIZE          2048
#define FSM_THREAD_PRIORITY     5

#define SENSOR_STACK_SIZE       1024
#define SENSOR_THREAD_PRIORITY  6

#define POLL_INTERVAL_MS        60      /* 16.67 Hz — matches your working blinky */
#define COOLDOWN_MS_DEFAULT     5000    /* 5s default; tune per deployment         */
#define SLEEP_INACTIVITY_MS     30000   /* 30s no event → deep sleep               */
#define MAX_FAULT_RETRIES       3

#define MOTION_THRESHOLD_G      1.5     /* Tune for vehicle vs rack deployment     */

/* ── Hardware globals ────────────────────────────────────────────────────── */

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);
static const struct device *mpu;
static struct sensor_value accel[3];

/* ── FSM states ──────────────────────────────────────────────────────────── */

typedef enum {
    STATE_BOOT,
    STATE_CALIBRATING,
    STATE_ARMED,
    STATE_LIGHT_SLEEP,
    STATE_DEEP_SLEEP,
    STATE_EVENT_DETECTED,
    STATE_SIGNING,
    STATE_TRANSMITTING,
    STATE_COOLDOWN,
    STATE_FAULT,
    STATE_LOW_BATTERY,
} lima_state_t;

static const char *state_name[] = {
    [STATE_BOOT]           = "BOOT",
    [STATE_CALIBRATING]    = "CALIBRATING",
    [STATE_ARMED]          = "ARMED",
    [STATE_LIGHT_SLEEP]    = "LIGHT SLEEP",
    [STATE_DEEP_SLEEP]     = "DEEP SLEEP",
    [STATE_EVENT_DETECTED] = "EVENT DETECTED",
    [STATE_SIGNING]        = "SIGNING",
    [STATE_TRANSMITTING]   = "TRANSMITTING",
    [STATE_COOLDOWN]       = "COOLDOWN",
    [STATE_FAULT]          = "FAULT",
    [STATE_LOW_BATTERY]    = "LOW BATTERY",
};

/* ── Message queue ───────────────────────────────────────────────────────── */

K_MSGQ_DEFINE(fsm_msgq, sizeof(lima_event_t), FSM_MSGQ_DEPTH, 4);

int lima_post_event(const lima_event_t *evt)
{
    return k_msgq_put(&fsm_msgq, evt, K_NO_WAIT);
}

/* ── FSM context ─────────────────────────────────────────────────────────── */

static struct {
    lima_state_t    current;
    lima_state_t    previous;
    lima_event_t    last_event;
    uint8_t         fault_retries;
    uint32_t        armed_since_ms;
    uint32_t        cooldown_ms;
} fsm;

/* ── Hardware drivers ────────────────────────────────────────────────────── */

/*
 * hw_init_sensors()
 * Real implementation — replaces stub.
 * Initializes MPU6050 and LED from device tree.
 */
static int hw_init_sensors(void)
{
    /* MPU6050 */
    mpu = DEVICE_DT_GET_ANY(invensense_mpu6050);
    if (!device_is_ready(mpu)) {
        LOG_ERR("MPU6050 not ready!");
        return -ENODEV;
    }
    LOG_INF("MPU6050 ready");

    /* LED heartbeat */
    if (!gpio_is_ready_dt(&led)) {
        LOG_ERR("LED not ready!");
        return -ENODEV;
    }
    gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);
    LOG_INF("LED ready");

    return 0;
}

/*
 * hw_read_imu()
 * Fetch a fresh accel sample from the MPU6050.
 * Returns magnitude in g, populates global accel[].
 */
static double hw_read_imu(void)
{
    sensor_sample_fetch(mpu);
    sensor_channel_get(mpu, SENSOR_CHAN_ACCEL_XYZ, accel);

    double ax = sensor_value_to_double(&accel[0]);
    double ay = sensor_value_to_double(&accel[1]);
    double az = sensor_value_to_double(&accel[2]);

    LOG_DBG("accel x:%.2f y:%.2f z:%.2f", ax, ay, az);

    return sqrt(ax*ax + ay*ay + az*az);
}

/* ── Remaining stubs (swap these in one at a time) ───────────────────────── */

static int hw_calibrate(void)
{
    /* TODO: collect baseline pressure + accel samples, store to SRAM */
    LOG_INF("[STUB] calibrate — using MPU6050 ready check as baseline");
    return 0;
}

static int hw_enable_irqs(void)
{
    /* TODO: configure MPU6050 interrupt pin via GPIO */
    LOG_INF("[STUB] enable_sensor_irqs");
    return 0;
}

static int hw_enter_light_sleep(void)
{
    /* TODO: disable non-critical peripherals via Zephyr PM */
    LOG_INF("[STUB] light_sleep");
    return 0;
}

static int hw_enter_deep_sleep(void)
{
    /* TODO: suspend all peripherals, BLE off, RTC wakeup only */
    LOG_INF("[STUB] deep_sleep");
    return 0;
}

static int hw_sign_event(lima_event_t *e)
{
    /* TODO: CryptoCell-310 ECDSA-P256 sign + per-event nonce */
    LOG_INF("[STUB] sign_event type=%d", e->type);
    return 0;
}

static int hw_ble_advertise(lima_event_t *e)
{
    /* TODO: BLE 5.0 Coded PHY advertisement with signed payload */
    LOG_INF("[STUB] ble_advertise type=%d", e->type);
    return 0;
}

static int hw_ble_stop(void)
{
    /* TODO: disable BLE stack for deep sleep */
    LOG_INF("[STUB] ble_stop");
    return 0;
}

static void hw_assert_fault_led(void)
{
    /* TODO: solid fault LED pattern (vs heartbeat blink) */
    LOG_INF("[STUB] assert_fault_led");
}

static int hw_try_recover(void)
{
    /* TODO: attempt I2C re-init x3 then watchdog reset */
    LOG_INF("[STUB] try_recover");
    return 0;
}

static void hw_watchdog_reset(void)
{
    /* TODO: trigger watchdog reset via Zephyr WDT API */
    LOG_INF("[STUB] watchdog_reset");
}

static void hw_notify_low_battery(void)
{
    /* TODO: BLE notify gateway of low battery state */
    LOG_INF("[STUB] notify_low_battery");
}

/* ── State transition helper ─────────────────────────────────────────────── */

static void transition(lima_state_t next)
{
    LOG_INF("FSM: %s -> %s", state_name[fsm.current], state_name[next]);
    fsm.previous = fsm.current;
    fsm.current  = next;
}

/* ── State handlers ──────────────────────────────────────────────────────── */

/*
 * STATE_BOOT
 * Init hardware, arm watchdog, load config.
 * On success -> CALIBRATING. On failure -> FAULT.
 */
static void state_boot_enter(void)
{
    LOG_INF("BOOT: initializing hardware");

    if (hw_init_sensors() != 0) {
        LOG_ERR("BOOT: hardware init failed -> FAULT");
        transition(STATE_FAULT);
        return;
    }

    LOG_INF("BOOT: init complete -> CALIBRATING");
    transition(STATE_CALIBRATING);
}

/*
 * STATE_CALIBRATING
 * Warm up sensors, establish baseline.
 * On success -> ARMED. On I2C error -> FAULT.
 */
static void state_calibrating_enter(void)
{
    LOG_INF("CALIBRATING: warming up sensors");

    int rc = hw_calibrate();
    if (rc != 0) {
        LOG_ERR("CALIBRATING: sensor error rc=%d -> FAULT", rc);
        transition(STATE_FAULT);
        return;
    }

    LOG_INF("CALIBRATING: baseline ready -> ARMED");
    transition(STATE_ARMED);
}

/*
 * STATE_ARMED
 * Active monitoring state.
 * Sensor thread handles the actual polling and posts events here.
 * Either sensor alone is sufficient to trigger EVENT_DETECTED.
 */
static void state_armed_enter(void)
{
    LOG_INF("ARMED: sensors active, watching for events");
    fsm.armed_since_ms = k_uptime_get_32();
    hw_enable_irqs();
}

static void state_armed_handle(const lima_event_t *evt)
{
    switch (evt->type) {
    case LIMA_EVT_PRESSURE_BREACH:
    case LIMA_EVT_MOTION_DETECTED:
    case LIMA_EVT_DUAL_BREACH:
    case LIMA_EVT_TAMPER_DETECTED:
        fsm.last_event = *evt;      /* copy event data before transitioning */
        transition(STATE_EVENT_DETECTED);
        break;

    case LIMA_EVT_POLL_TICK:
        transition(STATE_LIGHT_SLEEP);
        break;

    case LIMA_EVT_LOW_BATTERY:
    case LIMA_EVT_CRITICAL_BATTERY:
        transition(STATE_LOW_BATTERY);
        break;

    case LIMA_EVT_SENSOR_FAULT:
        fsm.fault_retries = 0;
        transition(STATE_FAULT);
        break;

    default:
        LOG_WRN("ARMED: unhandled event type=%d", evt->type);
        break;
    }
}

/*
 * STATE_LIGHT_SLEEP
 * Low power, sensor IRQs still active.
 * Returns to ARMED on any sensor candidate.
 * Falls to DEEP_SLEEP after inactivity threshold.
 */
static void state_light_sleep_enter(void)
{
    LOG_INF("LIGHT SLEEP: low-power, sensor IRQs active");
    hw_enter_light_sleep();
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
        if ((k_uptime_get_32() - fsm.armed_since_ms) > SLEEP_INACTIVITY_MS) {
            lima_event_t e = {
                .type         = LIMA_EVT_SLEEP_TIMER_EXPIRY,
                .timestamp_ms = k_uptime_get_32(),
            };
            lima_post_event(&e);
        } else {
            transition(STATE_ARMED);
        }
        break;

    case LIMA_EVT_SLEEP_TIMER_EXPIRY:
        transition(STATE_DEEP_SLEEP);
        break;

    case LIMA_EVT_LOW_BATTERY:
    case LIMA_EVT_CRITICAL_BATTERY:
        transition(STATE_LOW_BATTERY);
        break;

    default:
        LOG_WRN("LIGHT_SLEEP: unhandled event type=%d", evt->type);
        break;
    }
}

/*
 * STATE_DEEP_SLEEP
 * BLE off, all peripherals suspended.
 * RTC wakeup only -> back to ARMED.
 */
static void state_deep_sleep_enter(void)
{
    LOG_INF("DEEP SLEEP: BLE off, RTC wakeup only");
    hw_ble_stop();
    hw_enter_deep_sleep();
}

static void state_deep_sleep_handle(const lima_event_t *evt)
{
    switch (evt->type) {
    case LIMA_EVT_RTC_WAKEUP:
        LOG_INF("DEEP SLEEP: RTC wakeup -> ARMED");
        /* TODO: reinit BLE stack */
        transition(STATE_ARMED);
        break;

    case LIMA_EVT_LOW_BATTERY:
    case LIMA_EVT_CRITICAL_BATTERY:
        transition(STATE_LOW_BATTERY);
        break;

    default:
        LOG_WRN("DEEP_SLEEP: unhandled event type=%d", evt->type);
        break;
    }
}

/*
 * STATE_EVENT_DETECTED
 * Latch trigger type + timestamp, hand off to SIGNING.
 * The event data is already in fsm.last_event from the transition.
 */
static void state_event_detected_enter(void)
{
    LOG_INF("EVENT DETECTED: type=%d at t=%u ms",
            fsm.last_event.type,
            fsm.last_event.timestamp_ms);

    hw_sign_event(&fsm.last_event);

    /* In real impl, CryptoCell posts SIGNING_COMPLETE async.
     * Stub posts it synchronously for now. */
    lima_event_t e = {
        .type         = LIMA_EVT_SIGNING_COMPLETE,
        .timestamp_ms = k_uptime_get_32(),
    };
    lima_post_event(&e);

    transition(STATE_SIGNING);
}

/*
 * STATE_SIGNING
 * CryptoCell-310 ECDSA-P256 sign + nonce.
 * On completion -> TRANSMITTING.
 */
static void state_signing_enter(void)
{
    LOG_INF("SIGNING: waiting for CryptoCell completion");
}

static void state_signing_handle(const lima_event_t *evt)
{
    switch (evt->type) {
    case LIMA_EVT_SIGNING_COMPLETE:
        LOG_INF("SIGNING: payload ready -> TRANSMITTING");
        transition(STATE_TRANSMITTING);
        break;

    case LIMA_EVT_SENSOR_FAULT:
        LOG_WRN("SIGNING: fault during signing -> FAULT");
        transition(STATE_FAULT);
        break;

    default:
        LOG_WRN("SIGNING: unhandled event type=%d", evt->type);
        break;
    }
}

/*
 * STATE_TRANSMITTING
 * BLE 5.0 Coded PHY advertisement with signed payload.
 * On TX confirm -> COOLDOWN. On max retries -> FAULT.
 */
static void state_transmitting_enter(void)
{
    LOG_INF("TRANSMITTING: advertising signed payload via BLE");

    int rc = hw_ble_advertise(&fsm.last_event);
    if (rc != 0) {
        LOG_ERR("TRANSMITTING: BLE failed rc=%d -> FAULT", rc);
        lima_event_t e = {
            .type                   = LIMA_EVT_BLE_FAULT,
            .timestamp_ms           = k_uptime_get_32(),
            .data.fault.fault_code  = (uint8_t)rc,
        };
        lima_post_event(&e);
        return;
    }

    lima_event_t e = {
        .type         = LIMA_EVT_TX_COMPLETE,
        .timestamp_ms = k_uptime_get_32(),
    };
    lima_post_event(&e);
}

static void state_transmitting_handle(const lima_event_t *evt)
{
    switch (evt->type) {
    case LIMA_EVT_TX_COMPLETE:
        LOG_INF("TRANSMITTING: confirmed -> COOLDOWN");
        transition(STATE_COOLDOWN);
        break;

    case LIMA_EVT_BLE_FAULT:
        fsm.fault_retries = 0;
        transition(STATE_FAULT);
        break;

    default:
        LOG_WRN("TRANSMITTING: unhandled event type=%d", evt->type);
        break;
    }
}

/*
 * STATE_COOLDOWN
 * Suppress new events for configurable duration.
 * Prevents event storms (vehicle bounce, rack vibration).
 * TODO: replace k_sleep with k_work_delayable (non-blocking)
 */
static void state_cooldown_enter(void)
{
    uint32_t ms = fsm.cooldown_ms > 0 ? fsm.cooldown_ms : COOLDOWN_MS_DEFAULT;
    LOG_INF("COOLDOWN: suppressing events for %u ms", ms);

    k_sleep(K_MSEC(ms));   /* TODO: make non-blocking with k_work_delayable */

    lima_event_t e = {
        .type         = LIMA_EVT_COOLDOWN_EXPIRED,
        .timestamp_ms = k_uptime_get_32(),
    };
    lima_post_event(&e);
}

static void state_cooldown_handle(const lima_event_t *evt)
{
    switch (evt->type) {
    case LIMA_EVT_COOLDOWN_EXPIRED:
        LOG_INF("COOLDOWN: expired -> ARMED");
        transition(STATE_ARMED);
        break;

    case LIMA_EVT_LOW_BATTERY:
    case LIMA_EVT_CRITICAL_BATTERY:
        transition(STATE_LOW_BATTERY);
        break;

    default:
        /* Suppress sensor events during cooldown -- by design */
        LOG_DBG("COOLDOWN: suppressed event type=%d", evt->type);
        break;
    }
}

/*
 * STATE_FAULT
 * Log to flash, assert fault LED, attempt recovery.
 * Recovery success -> CALIBRATING.
 * Max retries exceeded -> watchdog reset.
 */
static void state_fault_enter(void)
{
    LOG_ERR("FAULT: entered (retry %d/%d)", fsm.fault_retries, MAX_FAULT_RETRIES);
    hw_assert_fault_led();

    /* TODO: log fault type to flash */
    /* TODO: broadcast fault over BLE if stack is up */

    if (fsm.fault_retries >= MAX_FAULT_RETRIES) {
        LOG_ERR("FAULT: max retries exceeded -> watchdog reset");
        hw_watchdog_reset();
        return;
    }

    int rc = hw_try_recover();
    fsm.fault_retries++;

    lima_event_t e = {
        .type         = (rc == 0) ? LIMA_EVT_RECOVERY_SUCCESS
                                   : LIMA_EVT_RECOVERY_FAILED,
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
        LOG_ERR("FAULT: recovery failed -> watchdog reset");
        hw_watchdog_reset();
        break;

    default:
        LOG_WRN("FAULT: unhandled event type=%d", evt->type);
        break;
    }
}

/*
 * STATE_LOW_BATTERY
 * Reduce poll rate, notify gateway, stay alert.
 * Restored -> ARMED. Critical -> shutdown.
 */
static void state_low_battery_enter(void)
{
    LOG_WRN("LOW BATTERY: reducing poll rate, notifying gateway");
    hw_notify_low_battery();
    /* TODO: reduce poll frequency */
    /* TODO: disable deep sleep cycling */
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
        /* TODO: safe shutdown sequence */
        break;

    case LIMA_EVT_PRESSURE_BREACH:
    case LIMA_EVT_MOTION_DETECTED:
    case LIMA_EVT_DUAL_BREACH:
    case LIMA_EVT_TAMPER_DETECTED:
        /* Still detect and report events even on low battery */
        fsm.last_event = *evt;
        transition(STATE_EVENT_DETECTED);
        break;

    default:
        LOG_WRN("LOW_BATTERY: unhandled event type=%d", evt->type);
        break;
    }
}

/* ── FSM dispatch ────────────────────────────────────────────────────────── */

static void fsm_dispatch(const lima_event_t *evt)
{
    static lima_state_t last_dispatched = (lima_state_t)-1;

    /* Run _enter() the first time we land in a new state */
    if (fsm.current != last_dispatched) {
        last_dispatched = fsm.current;
        switch (fsm.current) {
        case STATE_BOOT:            state_boot_enter();            break;
        case STATE_CALIBRATING:     state_calibrating_enter();     break;
        case STATE_ARMED:           state_armed_enter();           break;
        case STATE_LIGHT_SLEEP:     state_light_sleep_enter();     break;
        case STATE_DEEP_SLEEP:      state_deep_sleep_enter();      break;
        case STATE_EVENT_DETECTED:  state_event_detected_enter();  break;
        case STATE_SIGNING:         state_signing_enter();         break;
        case STATE_TRANSMITTING:    state_transmitting_enter();    break;
        case STATE_COOLDOWN:        state_cooldown_enter();        break;
        case STATE_FAULT:           state_fault_enter();           break;
        case STATE_LOW_BATTERY:     state_low_battery_enter();     break;
        }
        return; /* let the entry action settle before handling next event */
    }

    /* Route incoming events to the current state's handler */
    switch (fsm.current) {
    case STATE_ARMED:           state_armed_handle(evt);           break;
    case STATE_LIGHT_SLEEP:     state_light_sleep_handle(evt);     break;
    case STATE_DEEP_SLEEP:      state_deep_sleep_handle(evt);      break;
    case STATE_SIGNING:         state_signing_handle(evt);         break;
    case STATE_TRANSMITTING:    state_transmitting_handle(evt);    break;
    case STATE_COOLDOWN:        state_cooldown_handle(evt);        break;
    case STATE_FAULT:           state_fault_handle(evt);           break;
    case STATE_LOW_BATTERY:     state_low_battery_handle(evt);     break;
    default:
        /* BOOT / CALIBRATING / EVENT_DETECTED self-post their exit events */
        break;
    }
}

/* ── Sensor thread — real MPU6050 polling at 16.67 Hz ───────────────────── */

static void sensor_thread_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    LOG_INF("Sensor thread started");

    /* Wait for FSM to finish BOOT + CALIBRATING before polling */
    k_sleep(K_MSEC(500));

    while (1) {
        /* Only poll when FSM is in a watching state */
        if (fsm.current == STATE_ARMED || fsm.current == STATE_LIGHT_SLEEP) {

            double magnitude = hw_read_imu();

            /* Toggle LED as heartbeat — visible proof the node is alive */
            gpio_pin_toggle_dt(&led);

            if (magnitude > MOTION_THRESHOLD_G) {
                LOG_INF("MOTION: magnitude=%.2f g (threshold=%.2f)",
                        magnitude, MOTION_THRESHOLD_G);

                lima_event_t e = {
                    .type              = LIMA_EVT_MOTION_DETECTED,
                    .timestamp_ms      = k_uptime_get_32(),
                    .data.imu.accel_g  = (float)magnitude,
                    .data.imu.gyro_dps = 0.0f, /* TODO: add gyro channel read */
                };
                lima_post_event(&e);
            }
        }

        k_msleep(POLL_INTERVAL_MS); /* 60ms = 16.67 Hz */
    }
}

K_THREAD_DEFINE(sensor_thread, SENSOR_STACK_SIZE,
                sensor_thread_fn, NULL, NULL, NULL,
                SENSOR_THREAD_PRIORITY, 0, 0);

/* ── FSM thread ──────────────────────────────────────────────────────────── */

static void fsm_thread_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    LOG_INF("LIMA FSM thread started");

    fsm.current       = STATE_BOOT;
    fsm.fault_retries = 0;
    fsm.cooldown_ms   = COOLDOWN_MS_DEFAULT;

    /* Kick off the boot entry action directly */
    lima_event_t boot_evt = {
        .type         = LIMA_EVT_INIT_COMPLETE,
        .timestamp_ms = k_uptime_get_32(),
    };
    fsm_dispatch(&boot_evt);

    /* Main event loop — blocks on queue, wakes on each incoming event */
    lima_event_t evt;
    while (1) {
        int rc = k_msgq_get(&fsm_msgq, &evt, K_FOREVER);
        if (rc != 0) {
            LOG_ERR("msgq error rc=%d", rc);
            continue;
        }
        LOG_DBG("FSM recv event type=%d in state=%s",
                evt.type, state_name[fsm.current]);
        fsm_dispatch(&evt);
    }
}

K_THREAD_DEFINE(fsm_thread, FSM_STACK_SIZE,
                fsm_thread_fn, NULL, NULL, NULL,
                FSM_THREAD_PRIORITY, 0, 0);

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    LOG_INF("L.I.M.A. node firmware starting");
    LOG_INF("Board: nrf52840_mdk_usb_dongle -- NCS/Zephyr");
    /* Both threads are auto-started by K_THREAD_DEFINE above */
    return 0;
}
