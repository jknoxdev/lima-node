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

//  main.c
// ├── #includes + defines        (top)
// ├── hardware globals           (led, mpu, accel)
// ├── FSM states + handlers      (the big middle section)
// ├── fsm_dispatch()             
// ├── sensor_thread_fn()         
// ├── fsm_thread_fn()            
// ├── K_THREAD_DEFINE x2         (very bottom)
// └── main()

#include <zephyr/kernel.h>
#include <zephyr/kernel/thread.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>
#include <math.h>

#include "events.h"
#include <zephyr/drivers/gpio.h>

/* Use the pin numbers defined in your overlay */
#define I2C0_SCL_PIN 19
#define I2C0_SDA_PIN 20

LOG_MODULE_REGISTER(lima_fsm, LOG_LEVEL_DBG);

/* ── Forward Declarations ────────────────────────────────────────────────── */
int lima_post_event(const lima_event_t *evt);

/* ── Forward Declarations for State Handlers ────────────────────────────── */
static void state_boot_enter(void);
static void state_calibrating_enter(void);
static void state_armed_enter(void);
static void state_armed_exit(void);
static void state_event_detected_enter(void);
static void state_cooldown_enter(void);

/* ── Board definitions ───────────────────────────────────────────────────── */

// #define LED0_NODE DT_ALIAS(led0)
#define LED_R_NODE DT_ALIAS(led0)
#define LED_G_NODE DT_ALIAS(led1)
#define LED_B_NODE DT_ALIAS(led2)

/* ── Configuration ───────────────────────────────────────────────────────── */

#define FSM_MSGQ_DEPTH          16
// #define FSM_STACK_SIZE          2048
#define FSM_THREAD_PRIORITY     5

// #define SENSOR_STACK_SIZE       1024
#define SENSOR_THREAD_PRIORITY  6

#define POLL_INTERVAL_MS        60      /* 16.67 Hz — matches your working blinky */
#define COOLDOWN_MS_DEFAULT     5000    /* 5s default; tune per deployment         */
#define SLEEP_INACTIVITY_MS     30000   /* 30s no event → deep sleep               */
#define MAX_FAULT_RETRIES       3

#define MOTION_THRESHOLD_G      0.80     /* 1.1 good for table top     */

#define FSM_STACK_SIZE    8192  // Double it again
#define SENSOR_STACK_SIZE 4096  // Double it again

/* ── Hardware globals ────────────────────────────────────────────────────── */

// static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);
static const struct gpio_dt_spec led_r = GPIO_DT_SPEC_GET(LED_R_NODE, gpios);
static const struct gpio_dt_spec led_g = GPIO_DT_SPEC_GET(LED_G_NODE, gpios);
static const struct gpio_dt_spec led_b = GPIO_DT_SPEC_GET(LED_B_NODE, gpios);

static const struct device *mpu;
static struct sensor_value accel[3];
static struct k_work_delayable cooldown_work;

/* Non-blocking timer for cooldown */
static struct k_work_delayable cooldown_work;

/* Timer to blink LED in ARMED state */
void heartbeat_expiry_fn(struct k_timer *timer_id);
K_TIMER_DEFINE(heartbeat_timer, heartbeat_expiry_fn, NULL);

void heartbeat_expiry_fn(struct k_timer *timer_id)
{
    static uint8_t tick = 0;
    bool led_on = false;

    /* Pattern definition (Total 20 ticks = 2 seconds) 
     * Tick 0: ON,  Tick 1: OFF
     * Tick 2: ON,  Tick 3: OFF
     * Ticks 4-19: OFF (The 'wait' period)
     */
    if (tick == 0 || tick == 2) {
        led_on = true;
    } else {
        led_on = false;
    }

    gpio_pin_set_dt(&led_b, led_on ? 1 : 0);

    /* Increment and wrap every 20 ticks (20 * 100ms = 2 seconds) */
    tick = (tick + 1) % 20;

}

/* ── Work Queue Callback ─────────────────────────────────────────────────── */
/* This runs when the background timer expires  */
static void cooldown_expiry_cb(struct k_work *work)
{
    lima_event_t e = {
        .type         = LIMA_EVT_COOLDOWN_EXPIRED,
        .timestamp_ms = k_uptime_get_32(),
    };
    
    LOG_INF("COOLDOWN: timer expired, notifying FSM ");
    lima_post_event(&e);
}

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

    if (mpu == NULL) {
        LOG_ERR("MPU6050 device not found in devicetree!");
        return -ENODEV;
    }
    
    if (!device_is_ready(mpu)) {
        LOG_ERR("MPU6050 not ready!");
        return -ENODEV;
    }
    LOG_INF("MPU6050 ready");



    // /* LED heartbeat */
    // if (!gpio_is_ready_dt(&led)) {
    //     LOG_ERR("LED not ready!");
    //     return -ENODEV;
    // }
    // gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);
    // LOG_INF("LED ready");

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

     // Magnitude in m/s^2
    double mag_ms2 = sqrt(ax*ax + ay*ay + az*az);
    
    double current_g = mag_ms2 / 9.80665;

    // Normalize: Subtract Earth's gravity (approx 9.806)
    // and convert to Gs. This makes 'rest' approximately 0.0g.

    // magnitue
    // double mag_g = fabs((mag_ms2 / 9.80665) - 1.0);
    // LOG_DBG("accel x:%.2f y:%.2f z:%.2f | G:%.2f", ax, ay, az, mag_g);
    // return mag_g;

    // g's
    double motion_g = fabs(current_g - 1.0);
    LOG_DBG("Raw: %.2f m/s2 | Motion: %.2f G", mag_ms2, motion_g);
    return motion_g;
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

/* ── led blink -------------─────────────────────────────────────────────── */

// static void led_blink(int times)
// {
//     for (int i = 0; i < times; i++) {
//         gpio_pin_set_dt(&led, 0);   // active low = on
//         k_msleep(100);
//         gpio_pin_set_dt(&led, 1);   // off
//         k_msleep(100);
//     }
// }

static void hw_i2c_bus_recovery(void)
{
    LOG_INF("BOOT: Performing I2C bus recovery on P0.19/P0.20...");

    /* Get the device pointer for GPIO Port 0 */
    const struct device *gpio0_dev = DEVICE_DT_GET(DT_NODELABEL(gpio0));

    if (!device_is_ready(gpio0_dev)) {
        LOG_ERR("GPIO0 device not ready for recovery");
        return;
    }

    /* Configure SCL as output, SDA as input */
    gpio_pin_configure(gpio0_dev, I2C0_SCL_PIN, GPIO_OUTPUT_HIGH);
    gpio_pin_configure(gpio0_dev, I2C0_SDA_PIN, GPIO_INPUT);

    /* Clock out 9 pulses to release SDA */
    for (int i = 0; i < 9; i++) {
        gpio_pin_set_raw(gpio0_dev, I2C0_SCL_PIN, 1);
        k_busy_wait(5);
        gpio_pin_set_raw(gpio0_dev, I2C0_SCL_PIN, 0);
        k_busy_wait(5);
    }
    
    /* Leave SCL high (idle state) */
    gpio_pin_set_raw(gpio0_dev, I2C0_SCL_PIN, 1);
    
    /* 💡 CRITICAL: We must "unconfigure" the pins so the I2C driver 
       can take control of them again when it initializes later. */
    gpio_pin_configure(gpio0_dev, I2C0_SCL_PIN, GPIO_DISCONNECTED);
    gpio_pin_configure(gpio0_dev, I2C0_SDA_PIN, GPIO_DISCONNECTED);

    LOG_INF("BOOT: I2C recovery complete");
}

/* ── State transition helper ─────────────────────────────────────────────── */

// static void transition(lima_state_t next)
// {
//     LOG_INF("FSM: %s -> %s", state_name[fsm.current], state_name[next]);
//     fsm.previous = fsm.current;
//     fsm.current  = next;
// }

static void transition(lima_state_t next)
{
    LOG_INF("FSM: %s -> %s", state_name[fsm.current], state_name[next]);

    /* 1. EXIT ACTIONS: Clean up the old state before switching */
    switch (fsm.current) {
        case STATE_ARMED:
            state_armed_exit(); // 💡 Stops the heartbeat timer
            break;
        case STATE_COOLDOWN:
            /* If we leave cooldown early (e.g. Tamper), stop the work timer */
            k_work_cancel_delayable(&cooldown_work);
            break;
        default:
            break;
    }

    /* 2. STATE SWITCH */
    fsm.previous = fsm.current;
    fsm.current  = next;

    /* 3. ENTRY ACTIONS: Initialize the new state */
    switch (fsm.current) {
        case STATE_BOOT:
            state_boot_enter();
            break;
        case STATE_CALIBRATING:
            state_calibrating_enter();
            break;
        case STATE_ARMED:
            state_armed_enter(); // 💡 Starts the heartbeat timer
            break;
        case STATE_EVENT_DETECTED:
            state_event_detected_enter();
            break;
        case STATE_COOLDOWN:
            state_cooldown_enter();
            break;
        default:
            break;
    }
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

    /* White = R + G + B */
    gpio_pin_set_dt(&led_r, 1);
    gpio_pin_set_dt(&led_g, 1);
    gpio_pin_set_dt(&led_b, 1);

    k_work_init_delayable(&cooldown_work, cooldown_expiry_cb);

    k_msleep(100);

    if (hw_init_sensors() != 0) {
        LOG_ERR("BOOT: hardware init failed -> FAULT");
        transition(STATE_FAULT);
        return;
    }
    // led_blink(1);
    LOG_INF("BOOT: init complete -> CALIBRATING");
    k_msleep(50);   // let USB flush the log
    transition(STATE_CALIBRATING);
    LOG_INF("BOOT: transition called");  // ← does this print?
}

/*
 * STATE_CALIBRATING
 * Warm up sensors, establish baseline.
 * On success -> ARMED. On I2C error -> FAULT.
 */
static void state_calibrating_enter(void)
{
    LOG_INF("CALIBRATING: enter function reached");  // ← add this
    // led_blink(2);
    LOG_INF("CALIBRATING: warming up sensors");

    int rc = hw_calibrate();
    if (rc != 0) {
        LOG_ERR("CALIBRATING: sensor error rc=%d -> FAULT", rc);
        transition(STATE_FAULT);
        return;
    }

    LOG_INF("CALIBRATING: baseline ready -> ARMED");
    transition(STATE_ARMED);

    // /* kick FSM thread to run ARMED entry */
    // lima_event_t e = {
    //     .type         = LIMA_EVT_POLL_TICK,
    //     .timestamp_ms = k_uptime_get_32(),
    // };
    // lima_post_event(&e);
    LOG_INF("CALIBRATING: kicked event");

}

/*
 * STATE_ARMED
 * Active monitoring state.
 * Sensor thread handles the actual polling and posts events here.
 * Either sensor alone is sufficient to trigger EVENT_DETECTED.
 */
static void state_armed_enter(void)
{    
    // led_blink(3);
    LOG_INF("ARMED: Sensors active. Heartbeat started.");
    gpio_pin_set_dt(&led_r, 0);
    gpio_pin_set_dt(&led_g, 0);

    // /* Blink every 2 seconds (100ms on, then stays off until next cycle) */
    // k_timer_start(&heartbeat_timer, K_MSEC(2000), K_MSEC(2000));
    /* Timer now runs every 100ms to handle the rapid pulse pattern */
    k_timer_start(&heartbeat_timer, K_MSEC(100), K_MSEC(100));

    fsm.armed_since_ms = k_uptime_get_32();
    hw_enable_irqs();
}

static void state_armed_exit(void)
{
    k_timer_stop(&heartbeat_timer);
    // gpio_pin_set_dt(&led, 0); // Ensure LED is off when leaving ARMED
    gpio_pin_set_dt(&led_r, 0);
    gpio_pin_set_dt(&led_g, 0);
    gpio_pin_set_dt(&led_b, 0);
    LOG_DBG("ARMED: Heartbeat stopped.");
}

static void state_armed_handle(const lima_event_t *evt)
{
    // led_blink(3);
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
    case LIMA_EVT_INIT_COMPLETE:
    /* Ignore - already initialized */
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
    /* Heartbeat timer was stopped by state_armed_exit() automatically */
    /* Turn LED solid ON to indicate a 'Triggered' state */

    // gpio_pin_set_dt(&led, 1);

    /* Red Alert! */
    gpio_pin_set_dt(&led_r, 1);
    gpio_pin_set_dt(&led_g, 0);
    gpio_pin_set_dt(&led_b, 0);

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
    uint32_t ms = fsm.cooldown_ms > 0 ? fsm.cooldown_ms : 5000;
    LOG_INF("COOLDOWN: suppressing events for %u ms", ms);

    k_work_reschedule(&cooldown_work, K_MSEC(ms));
    
    // k_sleep(K_MSEC(ms));   /* TODO: make non-blocking with k_work_delayable */


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
        LOG_INF("COOLDOWN: expired -> ARMED [cite: 60]");
        transition(STATE_ARMED);
        break;

    case LIMA_EVT_LOW_BATTERY:
    case LIMA_EVT_TAMPER_DETECTED:
        /* 💡 ADDED: Immediate reaction to priority events  */
        LOG_WRN("COOLDOWN: priority event detected! Aborting timer.");
        k_work_cancel_delayable(&cooldown_work);
        
        if (evt->type == LIMA_EVT_TAMPER_DETECTED) {
            fsm.last_event = *evt;
            transition(STATE_EVENT_DETECTED);
        } else {
            transition(STATE_LOW_BATTERY);
        }
    //     break;
    // case LIMA_EVT_CRITICAL_BATTERY:
    //     transition(STATE_LOW_BATTERY);
    //     break;

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
    while (1) {

        static lima_state_t last_state = (lima_state_t)-1;

        /* Run entry if state changed */
        if (fsm.current != last_state) {
            last_state = fsm.current;

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

            /* If entry changed state again, loop and run new entry */
            continue;
        }

        break;  /* state stable */
    }

    /* Now route the incoming event */
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
        break;
    }
}

/* ── Sensor thread — real MPU6050 polling at 16.67 Hz ───────────────────── */

static void sensor_thread_fn(void *p1, void *p2, void *p3)
{
    LOG_INF("FSM sensor thread resumed!");  // ← add this as very first line
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    LOG_INF("Sensor thread started");

    /* Wait for FSM to finish BOOT + CALIBRATING before polling */
    k_sleep(K_MSEC(500));

    while (1) {
        /* Only poll when FSM is in a watching state */
        if (fsm.current == STATE_ARMED || fsm.current == STATE_LIGHT_SLEEP) {

            double magnitude = hw_read_imu();

            /* Toggle LED as heartbeat — visible proof the node is alive */
            // gpio_pin_toggle_dt(&led);

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

/* ── FSM thread ──────────────────────────────────────────────────────────── */

static void fsm_thread_fn(void *p1, void *p2, void *p3)
{
    LOG_INF("FSM thread resumed!");  // ← add this as very first line
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
    // fsm_dispatch(&boot_evt);
    lima_post_event(&boot_evt);

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

//  * Thread startup:
//  *   Threads are defined with K_FOREVER (no auto-start).
//  *   main() blinks LED to let USB settle, then calls:
//  *       k_thread_start(fsm_thread);
//  *       k_thread_start(sensor_thread);
//  *   This prevents the USB suspend/reset cycle from
//  *   interrupting FSM boot sequencing.

K_THREAD_DEFINE(fsm_thread, FSM_STACK_SIZE,
                fsm_thread_fn, NULL, NULL, NULL,
                FSM_THREAD_PRIORITY, 0, 0);

K_THREAD_DEFINE(sensor_thread, SENSOR_STACK_SIZE,
                sensor_thread_fn, NULL, NULL, NULL,
                SENSOR_THREAD_PRIORITY, 0, 0);

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    // suspend threads immediately before USB chaos starts
    k_thread_suspend(fsm_thread);
    k_thread_suspend(sensor_thread);

    // 2. Clear the I2C bus manually before doing ANYTHING else
    // This prevents a hung sensor from blocking the driver later
    hw_i2c_bus_recovery();

    /* Init RGB Pins */
    gpio_pin_configure_dt(&led_r, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&led_g, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&led_b, GPIO_OUTPUT_INACTIVE);

    // /* Temporary test: Force the LED to stay on regardless of FSM */
    // gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE); 
    // gpio_pin_set_dt(&led, 1); // If ACTIVE_LOW is set in DTS, this should light it up.
    
    LOG_INF("L.I.M.A. node firmware starting");
    
    // 3. Increase the wait and ensure USB is stable
    // We wait 6 seconds now to be absolutely sure the host has finished enumeration
    for (int i = 0; i < 6; i++) {
        // led_blink(1);
        k_msleep(1000);
        LOG_INF("USB Settle Loop: %d/6", i+1);
    }
    
    LOG_INF("Starting LIMA Threads...");
    k_thread_resume(fsm_thread);
    k_thread_resume(sensor_thread);

    return 0;
}
