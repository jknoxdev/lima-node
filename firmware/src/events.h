/*
 * L.I.M.A. — Local Integrity Multi-modal Architecture
 * events.h — Event types and payload struct
 *
 * All inter-thread communication flows through lima_event.
 * ISRs and sensor threads post to the FSM message queue;
 * the FSM thread consumes and transitions state.
 */

#ifndef LIMA_EVENTS_H
#define LIMA_EVENTS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Event types ──────────────────────────────────────────────────────────── */

typedef enum {
    /* Sensor triggers */
    LIMA_EVT_PRESSURE_BREACH    = 0x01,  /* BMP280: pressure delta > threshold  */
    LIMA_EVT_MOTION_DETECTED    = 0x02,  /* MPU6050: acceleration > threshold   */
    LIMA_EVT_DUAL_BREACH        = 0x03,  /* Both sensors triggered              */

    /* Node health */
    LIMA_EVT_TAMPER_DETECTED    = 0x04,  /* Case open / voltage spike           */
    LIMA_EVT_LOW_BATTERY        = 0x05,  /* Vbat < low threshold                */
    LIMA_EVT_CRITICAL_BATTERY   = 0x06,  /* Vbat < critical threshold → shutdown*/
    LIMA_EVT_BATTERY_RESTORED   = 0x07,  /* Vbat recovered                      */

    /* Sensor / hardware faults */
    LIMA_EVT_SENSOR_FAULT       = 0x08,  /* I2C error / sensor dropout          */
    LIMA_EVT_BLE_FAULT          = 0x09,  /* BLE TX failed (max retries)         */
    LIMA_EVT_RECOVERY_SUCCESS   = 0x0A,  /* Fault recovery succeeded            */
    LIMA_EVT_RECOVERY_FAILED    = 0x0B,  /* Unrecoverable → watchdog reset      */

    /* Lifecycle / timing */
    LIMA_EVT_INIT_COMPLETE      = 0x10,  /* Boot init done, watchdog armed      */
    LIMA_EVT_BASELINE_READY     = 0x11,  /* Calibration complete                */
    LIMA_EVT_POLL_TICK          = 0x12,  /* Poll interval elapsed (no event)    */
    LIMA_EVT_SLEEP_TIMER_EXPIRY = 0x13,  /* Inactivity → deep sleep             */
    LIMA_EVT_RTC_WAKEUP         = 0x14,  /* RTC woke from deep sleep            */
    LIMA_EVT_COOLDOWN_EXPIRED   = 0x15,  /* Cooldown timer done → rearm         */
    LIMA_EVT_TX_COMPLETE        = 0x16,  /* BLE advertisement confirmed         */
    LIMA_EVT_SIGNING_COMPLETE   = 0x17,  /* Payload signed and ready            */

} lima_event_type_t;

/* ── Sensor payload variants ─────────────────────────────────────────────── */

typedef struct {
    float   delta_pa;        /* Pressure delta from baseline (Pascals) */
    float   abs_hpa;         /* Absolute reading (hPa)                 */
} lima_baro_data_t;

typedef struct {
    float   accel_g;         /* Peak acceleration magnitude (g)        */
    float   gyro_dps;        /* Peak gyro magnitude (deg/s)            */
} lima_imu_data_t;

typedef struct {
    uint8_t fault_code;      /* Hardware fault identifier              */
    uint8_t retry_count;     /* How many recovery attempts so far      */
} lima_fault_data_t;

typedef struct {
    uint16_t mv;             /* Battery voltage in millivolts          */
} lima_battery_data_t;

/* ── Master event struct ─────────────────────────────────────────────────── */

typedef struct {
    lima_event_type_t   type;
    uint32_t            timestamp_ms;   /* k_uptime_get_32() at event creation */
    uint8_t             node_id[6];     /* BLE MAC address of this node        */

    union {
        lima_baro_data_t    baro;
        lima_imu_data_t     imu;
        lima_fault_data_t   fault;
        lima_battery_data_t battery;
    } data;
} lima_event_t;

#ifdef __cplusplus
}
#endif

#endif /* LIMA_EVENTS_H */
