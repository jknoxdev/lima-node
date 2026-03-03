/*
 * ============================================================================
 * Project: L.I.M.A. (Local Integrity Multi-modal Architecture)
 * File:    main.c
 * Author:  Justin Knox
 * Date:    Feb 2026
 *
 * Description: 
 * Event-driven FSM for edge integrity monitoring. Uses a decoupled 
 * thread model to separate high-frequency sensor polling from 
 * cryptographic signing and BLE transmission states.
 *
 * Architecture:
 * ├── #includes & defines     (System & Zephyr RTOS config)
 * ├── Hardware Globals        (LEDs, MPU6050, Message Queues)
 * ├── HAL & Stubs             (I2C Recovery, IMU Read, Crypto/BLE stubs)
 * ├── FSM Engine              (State transitions & Entry/Exit logic)
 * ├── fsm_dispatch()          (Event-to-State routing)
 * ├── Thread Functions        (sensor_thread_fn, fsm_thread_fn)
 * ├── K_THREAD_DEFINE x2      (Static thread allocation)
 * └── main()                  (Hardware init & Thread resumption)
 * ============================================================================
 */

#include <zephyr/kernel.h>
#include <zephyr/kernel/thread.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>
#include <math.h>
#include "events.h"
#include "fsm.h"

LOG_MODULE_REGISTER(lima_main, LOG_LEVEL_DBG);

#define I2C0_SCL_PIN 19
#define I2C0_SDA_PIN 20

/* ── Board definitions ───────────────────────────────────────────────────── */

#define LED_R_NODE DT_ALIAS(led0)
#define LED_G_NODE DT_ALIAS(led1)
#define LED_B_NODE DT_ALIAS(led2)

/* ── Configuration ───────────────────────────────────────────────────────── */

#define FSM_MSGQ_DEPTH          16
#define FSM_THREAD_PRIORITY     5
#define SENSOR_THREAD_PRIORITY  6
#define POLL_INTERVAL_MS        60      /* 16.67 Hz — matches your working blinky */
#define COOLDOWN_MS_DEFAULT     5000    /* 5s default; tune per deployment         */
#define MOTION_THRESHOLD_G      0.80     /* 1.1 good for table top     */
#define FSM_STACK_SIZE          8192  // Double it again
#define SENSOR_STACK_SIZE       4096  // Double it again



/* ── Hardware globals ────────────────────────────────────────────────────── */

static const struct gpio_dt_spec led_r = GPIO_DT_SPEC_GET(LED_R_NODE, gpios);
static const struct gpio_dt_spec led_g = GPIO_DT_SPEC_GET(LED_G_NODE, gpios);
static const struct gpio_dt_spec led_b = GPIO_DT_SPEC_GET(LED_B_NODE, gpios);

static const struct device *mpu;
static struct sensor_value accel[3];

/* ── Message queue ───────────────────────────────────────────────────────── */

K_MSGQ_DEFINE(fsm_msgq, sizeof(lima_event_t), FSM_MSGQ_DEPTH, 4);

int lima_post_event(const lima_event_t *evt)
{
    return k_msgq_put(&fsm_msgq, evt, K_NO_WAIT);
}

/* Non-blocking timer for cooldown */
static struct k_work_delayable cooldown_work; 

/* Non-blocking timer tx timeout */
static struct k_work_delayable tx_timeout_work;

/* Threads */
static void sensor_thread_fn(void *p1, void *p2, void *p3);
static void fsm_thread_fn(void *p1, void *p2, void *p3);


/* ── Heartbeat timer (ARMED state blue pulse) ────────────────────────────── */

static void heartbeat_expiry_fn(struct k_timer *timer_id);
K_TIMER_DEFINE(heartbeat_timer, heartbeat_expiry_fn, NULL);

static void heartbeat_expiry_fn(struct k_timer *timer_id)
{
    ARG_UNUSED(timer_id);
    static uint8_t tick = 0;

    /* Double-blink pattern: on at tick 0 and 2, off otherwise.
     * Period = 20 ticks × 100 ms = 2 seconds. */
    bool led_on = (tick == 0 || tick == 2);
    gpio_pin_set_dt(&led_b, led_on ? 1 : 0);
    tick = (tick + 1) % 20;
}

/* ── Hardware Abstraction Layer ──────────────────────────────────────────── */

static int hw_init_sensors(void)
{
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
    return 0;
}

static double hw_read_imu(void)
{
    sensor_sample_fetch(mpu);
    sensor_channel_get(mpu, SENSOR_CHAN_ACCEL_XYZ, accel);

    double ax = sensor_value_to_double(&accel[0]);
    double ay = sensor_value_to_double(&accel[1]);
    double az = sensor_value_to_double(&accel[2]);

    double mag_ms2   = sqrt(ax * ax + ay * ay + az * az);
    double current_g = mag_ms2 / 9.80665;
    double motion_g  = fabs(current_g - 1.0);

    LOG_DBG("Raw: %.2f m/s2 | Motion: %.2f G", mag_ms2, motion_g);
    return motion_g;
}

static void hw_i2c_bus_recovery(void)
{
    LOG_INF("BOOT: I2C bus recovery on P0.%d/P0.%d", I2C0_SCL_PIN, I2C0_SDA_PIN);

    const struct device *gpio0_dev = DEVICE_DT_GET(DT_NODELABEL(gpio0));
    if (!device_is_ready(gpio0_dev)) {
        LOG_ERR("GPIO0 device not ready for recovery");
        return;
    }

    gpio_pin_configure(gpio0_dev, I2C0_SCL_PIN, GPIO_OUTPUT_HIGH);
    gpio_pin_configure(gpio0_dev, I2C0_SDA_PIN, GPIO_INPUT);

    for (int i = 0; i < 9; i++) {
        gpio_pin_set_raw(gpio0_dev, I2C0_SCL_PIN, 1);
        k_busy_wait(5);
        gpio_pin_set_raw(gpio0_dev, I2C0_SCL_PIN, 0);
        k_busy_wait(5);
    }
    gpio_pin_set_raw(gpio0_dev, I2C0_SCL_PIN, 1);

    gpio_pin_configure(gpio0_dev, I2C0_SCL_PIN, GPIO_DISCONNECTED);
    gpio_pin_configure(gpio0_dev, I2C0_SDA_PIN, GPIO_DISCONNECTED);

    LOG_INF("BOOT: I2C recovery complete");
}


/* ── HAL Stubs (swap in real implementations one at a time) ──────────────── */

static int hw_calibrate(void)
{
    LOG_INF("[STUB] calibrate");
    return 0;
}

static int hw_enable_irqs(void)
{
    LOG_INF("[STUB] enable_sensor_irqs");
    return 0;
}

static int hw_enter_light_sleep(void)
{
    LOG_INF("[SLEEP] entering light sleep — heartbeat interval %d ms", 
            CONFIG_LIMA_LIGHT_SLEEP_HEARTBEAT_MS);
    // TODO: real PM suspend goes here
    k_msleep(CONFIG_LIMA_LIGHT_SLEEP_HEARTBEAT_MS);
    LOG_INF("[SLEEP] light sleep expired — waking");
    return 0;
}

static int hw_enter_deep_sleep(void)
{
    LOG_INF("[SLEEP] entering deep sleep — interval %d ms",
            CONFIG_LIMA_DEEP_SLEEP_INTERVAL_MS);
    // TODO: real PM_STATE_SOFT_OFF goes here
    k_msleep(CONFIG_LIMA_DEEP_SLEEP_INTERVAL_MS);
    LOG_INF("[SLEEP] deep sleep expired — waking");
    return 0;
}

static int hw_sign_event(lima_event_t *e)
{
    LOG_INF("[STUB] sign_event type=0x%02X", e->type);
    return 0;
}

static int hw_ble_advertise(lima_event_t *e)
{
    LOG_INF("[STUB] ble_advertise type=0x%02X", e->type);
    return 0;
}

static int hw_ble_stop(void)
{
    LOG_INF("[STUB] ble_stop");
    return 0;
}

static void hw_assert_fault_led(void)
{
    LOG_INF("[STUB] assert_fault_led");
}

static int hw_try_recover(void)
{
    LOG_INF("[STUB] try_recover");
    return 0;
}

static void hw_watchdog_reset(void)
{
    LOG_INF("[STUB] watchdog_reset");
}

static void hw_notify_low_battery(void)
{
    LOG_INF("[STUB] notify_low_battery");
}

/* Suppress "defined but not used" warnings for stubs not yet wired */
static void __attribute__((unused)) suppress_stub_warnings(void)
{
    (void)hw_calibrate;
    (void)hw_enable_irqs;
    (void)hw_sign_event;
    (void)hw_ble_advertise;
    (void)hw_ble_stop;
    (void)hw_assert_fault_led;
    (void)hw_try_recover;
    (void)hw_watchdog_reset;
    (void)hw_notify_low_battery;
}

/* ── fsm_hw_* Trampolines (called by fsm.c) ──────────────────────────────── */

void fsm_hw_enter_sleep(void)
{
    hw_enter_light_sleep();
}

void fsm_hw_enter_deep_sleep(void)
{
    hw_enter_deep_sleep();
    hw_ble_stop();
}

/**
 * @brief Physical hardware response to FSM state changes.
 *        Called by fsm.c during every transition.
 */
void fsm_hw_set_led(lima_state_t state)
{
    /* Stop the heartbeat before changing LEDs; restart it only for ARMED */
    k_timer_stop(&heartbeat_timer);

    gpio_pin_set_dt(&led_r, 0);
    gpio_pin_set_dt(&led_g, 0);
    gpio_pin_set_dt(&led_b, 0);

    switch (state) {
    case STATE_BOOT:
    case STATE_CALIBRATING:
        /* White (R+G+B) during init */
        gpio_pin_set_dt(&led_r, 1);
        gpio_pin_set_dt(&led_g, 1);
        gpio_pin_set_dt(&led_b, 1);
        break;

    case STATE_ARMED:
        /* Blue double-blink heartbeat */
        k_timer_start(&heartbeat_timer, K_MSEC(100), K_MSEC(100));
        break;

    case STATE_EVENT_DETECTED:
    case STATE_SIGNING:
    case STATE_TRANSMITTING:
        /* Solid Red during active alert pipeline */
        gpio_pin_set_dt(&led_r, 1);
        break;

    case STATE_COOLDOWN:
        /* Yellow (R+G) during suppression window */
        gpio_pin_set_dt(&led_r, 1);
        gpio_pin_set_dt(&led_g, 1);
        break;

    case STATE_FAULT:
        /* Solid Red + log */
        gpio_pin_set_dt(&led_r, 1);
        LOG_ERR("HARDWARE: Fault LED asserted");
        break;

    case STATE_LOW_BATTERY:
        /* Dim amber: just Red at low duty — use solid for simplicity now */
        gpio_pin_set_dt(&led_r, 1);
        break;

    case STATE_LIGHT_SLEEP:
        /* 3 quick purple blinks */
        for (int i = 0; i < 3; i++) {
            gpio_pin_set_dt(&led_r, 1);
            gpio_pin_set_dt(&led_b, 1);
            k_msleep(100);
            gpio_pin_set_dt(&led_r, 0);
            gpio_pin_set_dt(&led_b, 0);
            k_msleep(100);
        }
        break;
        
    case STATE_DEEP_SLEEP:
        /* 4 quick purple blinks */
        for (int i = 0; i < 4; i++) {
            gpio_pin_set_dt(&led_r, 1);
            gpio_pin_set_dt(&led_b, 1);
            k_msleep(100);
            gpio_pin_set_dt(&led_r, 0);
            gpio_pin_set_dt(&led_b, 0);
            k_msleep(100);
        }
        break;

    default:
        /* Dark for sleep states */
        break;
    }
}

/* ── Sensor Thread ───────────────────────────────────────────────────────── */

static void sensor_thread_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);
    LOG_INF("Sensor thread started... sleeping 500ms for FSM boot & calibration...");

    /* Wait for FSM to finish BOOT + CALIBRATING */
    k_sleep(K_MSEC(500));

    LOG_INF("Sensor thread: 500ms expired...");


    while (1) {
        lima_state_t s = fsm_get_state();
        LOG_INF("Sensor thread: gathering FSM state...");
        if (s == STATE_ARMED || s == STATE_LIGHT_SLEEP) {
            double magnitude = hw_read_imu();

            if (magnitude > MOTION_THRESHOLD_G) {
                LOG_INF("MOTION: %.2f g (threshold=%.2f)", magnitude, MOTION_THRESHOLD_G);

                lima_event_t e = {
                    .type              = LIMA_EVT_MOTION_DETECTED,
                    .timestamp_ms      = k_uptime_get_32(),
                    .data.imu.accel_g  = (float)magnitude,
                    .data.imu.gyro_dps = 0.0f,
                };
                lima_post_event(&e);
            }
        }

        k_msleep(POLL_INTERVAL_MS);
    }
}

/* ── FSM thread ──────────────────────────────────────────────────────────── */

static void fsm_thread_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);
    LOG_INF("FSM thread started");

    fsm_init();

    lima_event_t msg;
    while (1) {
        if (k_msgq_get(&fsm_msgq, &msg, K_FOREVER) == 0) {
            fsm_dispatch(&msg);
        }
    }
}

K_THREAD_DEFINE(fsm_thread, FSM_STACK_SIZE,
                fsm_thread_fn, NULL, NULL, NULL,
                FSM_THREAD_PRIORITY, 0, 0);

K_THREAD_DEFINE(sensor_thread, SENSOR_STACK_SIZE,
                sensor_thread_fn, NULL, NULL, NULL,
                SENSOR_THREAD_PRIORITY, 0, 0);
                
/* ── main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    LOG_INF("L.I.M.A. node firmware starting");
    LOG_INF("L.I.M.A.: suspending threads...");

    k_thread_suspend(fsm_thread);
    k_thread_suspend(sensor_thread);
    
    gpio_pin_configure_dt(&led_r, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&led_g, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&led_b, GPIO_OUTPUT_INACTIVE);

    hw_i2c_bus_recovery();

    if (hw_init_sensors() != 0) {
        LOG_ERR("Sensor init failed!");
        // post SENSOR_FAULT event or spin
    }



    for (int i = 0; i < 6; i++) {
        k_msleep(1000);
        LOG_INF("USB settle: %d/6", i + 1);
    }

    LOG_INF("Starting LIMA threads...");
    k_thread_resume(fsm_thread);
    k_thread_resume(sensor_thread);

    return 0;
}
