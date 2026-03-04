#ifndef LIMA_FSM_H
#define LIMA_FSM_H

#include <zephyr/kernel.h>
#include <stdbool.h>
#include "events.h"

#define SLEEP_INACTIVITY_MS     10000   /* 30s no event → deep sleep               */
#define TX_TIMEOUT_MS           1500     /* tune later */
#define MAX_FAULT_RETRIES       3
#define ARMED_DWELL_MS          5000

/* ── State Definitions ───────────────────────────────────────────────────── */
typedef enum {
    STATE_BOOT,
    STATE_CALIBRATING,
    STATE_ARMED,
    STATE_LIGHT_SLEEP,    /* System ON: CPU idle, RAM on, fast wakeup */
    STATE_DEEP_SLEEP,     /* System OFF: Lowest power, RAM off, Reset wakeup */
    STATE_EVENT_DETECTED,
    STATE_SIGNING,
    STATE_TRANSMITTING,
    STATE_COOLDOWN,
    STATE_FAULT,
    STATE_LOW_BATTERY,
    STATE_COUNT
} lima_state_t;

/* ── FSM Context ─────────────────────────────────────────────────────────── */
/* Holds all mutable FSM state. Defined in fsm.c, declared extern here. */
typedef struct {
    lima_event_t    last_event;         /* Latched trigger event */
    uint32_t        armed_since_ms;     /* k_uptime when we entered ARMED */
    uint32_t        cooldown_ms;        /* Configurable cooldown duration */
    uint8_t         fault_retries;      /* Retry counter for FAULT recovery */
} lima_fsm_ctx_t;


/* ── FSM Logic API ───────────────────────────────────────────────────────── */

void fsm_init(void);
void fsm_dispatch(const lima_event_t *evt); 
lima_state_t fsm_get_state(void);
const char* fsm_state_to_str(lima_state_t state);

/* ── Event Queue API (defined in main.c) ────────────────────────────────── */
/* fsm.c calls this to post events back to the queue during transitions */
int lima_post_event(const lima_event_t *evt);


/* ── Hardware Abstraction Hooks (The "Stubs" for main.c) ────────────────── */

/**
 * @brief Light Sleep (System ON). 
 * CPU stops, but peripheral state and RAM are preserved.
 */
void fsm_hw_enter_sleep(void);

/**
 * @brief Deep Sleep (System OFF). 
 * chip powers down almost entirely. Next wakeup is a reboot.
 */
void fsm_hw_enter_deep_sleep(void);

/**
 * @brief Updates physical LEDs based on the FSM state.
 */
void fsm_hw_set_led(lima_state_t state);

#endif /* LIMA_FSM_H */