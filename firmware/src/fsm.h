/*
 * L.I.M.A. — Local Integrity Multi-modal Architecture
 * fsm.h — FSM public API, state definitions, and context struct
 *
 * Wire format spec: docs/dev/frame-record-spec.md
 */

#ifndef LIMA_FSM_H
#define LIMA_FSM_H

#include <stdint.h>
#include "events.h"
#include "crypto.h"
#include "ble.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Timing constants ────────────────────────────────────────────────────── */

#define ARMED_DWELL_MS          10000   /* ms in ARMED before light sleep eligible */
#define SLEEP_INACTIVITY_MS     86400000 /* 24h — bumped for first endurance test  */
#define TX_TIMEOUT_MS           5000    /* ms to wait for BLE TX confirmation      */
#define MAX_FAULT_RETRIES       3       /* fault recovery attempts before WDT reset */

/* ── State machine states ────────────────────────────────────────────────── */

typedef enum {
    STATE_BOOT           = 0,
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
    STATE_COUNT,
} lima_state_t;

/* ── FSM context ─────────────────────────────────────────────────────────── */
/*
 * Single instance — lives in fsm.c as `lima_fsm_ctx_t fsm`.
 * All signing and transmission state threads through here.
 *
 * last_event  — raw lima_event_t from sensor thread; copied on trigger
 * last_ler    — LIMA Event Record (LER, 24B) built from last_event
 * last_lf     — LIMA Frame (LF, 184B) assembled in signing_complete_cb
 *               STUB(feat/frame-record-spec): ciphertext = plaintext until
 *               AES-256-GCM is wired in feat/ler-encrypt
 */
typedef struct {
    lima_event_t    last_event;         /* raw event from sensor thread     */
    lima_ler_t      last_ler;           /* LER built from last_event        */
    lima_lf_t       last_lf;            /* LF assembled after signing       */

    uint32_t        cooldown_ms;        /* cooldown suppression window (ms) */
    uint32_t        armed_since_ms;     /* uptime at last ARMED entry       */
    int             fault_retries;      /* fault recovery attempt counter   */
} lima_fsm_ctx_t;

/* ── Public API ──────────────────────────────────────────────────────────── */

/**
 * @brief Initialize FSM work items and enter BOOT state.
 *
 * Must be called from fsm_thread_fn() before the event loop.
 */
void fsm_init(void);

/**
 * @brief Dispatch an event to the active state's handler.
 *
 * Called from the FSM thread event loop for every queued lima_event_t.
 * Kicks the hardware watchdog on every call.
 *
 * @param evt  Event to dispatch — must be non-NULL.
 */
void fsm_dispatch(const lima_event_t *evt);

/**
 * @brief Return the current FSM state.
 */
lima_state_t fsm_get_state(void);

/**
 * @brief Return a human-readable string for a state value.
 */
const char *fsm_state_to_str(lima_state_t state);

/* ── Event posting (implemented in main.c) ───────────────────────────────── */

/**
 * @brief Post an event to the FSM message queue.
 *
 * Non-blocking. May be called from ISR or thread context.
 * Returns 0 on success, negative errno if queue is full.
 */
int lima_post_event(const lima_event_t *evt);

/* ── Hardware hooks (implemented in main.c) ──────────────────────────────── */

/**
 * @brief Set LEDs to reflect the current FSM state.
 */
void fsm_hw_set_led(lima_state_t state);

/**
 * @brief Enter light sleep mode (CPU idle, sensor IRQs active).
 */
void fsm_hw_enter_sleep(void);

/**
 * @brief Enter deep sleep (BLE off, RTC wakeup only).
 */
void fsm_hw_enter_deep_sleep(void);

/**
 * @brief Feed the hardware watchdog timer.
 *
 * Called at the top of every fsm_dispatch() invocation.
 */
void fsm_hw_wdt_kick(void);

/* ── Global FSM context (defined in fsm.c) ───────────────────────────────── */

extern lima_fsm_ctx_t fsm;

#ifdef __cplusplus
}
#endif

#endif /* LIMA_FSM_H */
