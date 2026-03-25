# LIMA — RTC Refactor Context

> **Created:** 2026-03-24
> **Branch:** `feat/rtc-refactor`
> **Purpose:** Summary of the DS3231 RTC extraction out of `main.c` — where it was, where it landed, and what still needs doing.

---

## What Was the Starting Point

Before PR #14 (`feat(rtc): RTC integration`), the RTC wakeup was a **software stub** in `main.c`. No hardware was involved:

```c
/* main.c (pre-#14) */
static struct k_work_delayable rtc_wakeup_work;

static void rtc_wakeup_expiry_fn(struct k_work *work) {
    lima_event_t e = { .type = LIMA_EVT_RTC_WAKEUP, ... };
    lima_post_event(&e);
}

void fsm_hw_enter_deep_sleep(void) {
    k_work_reschedule(&rtc_wakeup_work, K_MSEC(CONFIG_LIMA_DEEP_SLEEP_INTERVAL_MS));
}
```

Sleep was a software delay. The FSM waited for `LIMA_EVT_RTC_WAKEUP` but that event came from a kernel timer, not the DS3231.

---

## What PR #14 Did (March 22–23)

Wired the physical DS3231 into the firmware in-place inside `main.c`:

- Added `ds3231@68` device tree node to overlay files
- Added `CONFIG_COUNTER=y`, `CONFIG_MAXIM_DS3231=y` to `prj.conf`
- Added `hw_init_rtc()` function that called `counter_start()` and `counter_set_channel_alarm()`
- Added GPIO interrupt test on INT/SQW pin (P0.28 on DK)
- Root-caused and documented the DS3231 alarm flag (AF bit) not being cleared on boot — the interrupt storm bug
- Added `CONFIG_LIMA_PROVISION_UNIX_TIME` Kconfig symbol for build-time epoch bake-in
- Added `firmware/tools/provision.py` for time provisioning
- Added `docs/dev/context/context-rtc-swap.md` — full DS1307→DS3231 migration reference

At this stage, all RTC logic lived in `main.c` directly (~150–200 lines added there).

---

## What the `feat/rtc-refactor` Branch Did (March 23)

Three commits extracted and hardened the RTC code:

### `6d5adb0` — add rtc.c and rtc.h

Created `firmware/src/rtc.c` (383 lines) and `firmware/src/rtc.h` (137 lines) with a complete module API:

| Function | Purpose |
|---|---|
| `lima_rtc_init()` | Boot sequence: read DS3231, compare NVS, resolve fallback chain, anchor epoch |
| `lima_rtc_get_epoch()` | Return unix seconds (boot_epoch + elapsed uptime) |
| `lima_rtc_timestamp_ms()` | Wall-clock ms for `lima_payload_t` (replaces `k_uptime_get_32()`) |
| `lima_rtc_set_epoch()` | Write epoch to DS3231 + NVS + re-anchor (gateway time-sync path) |
| `lima_rtc_flush()` | Flush current epoch to Settings immediately (pre-sleep) |
| `lima_rtc_arm_wakeup()` | Arm DS3231 counter alarm; falls back to `k_work_reschedule` if RTC invalid |
| `lima_rtc_is_valid()` | Returns whether a live hardware or NVS source was found |

The module also implements:
- Three-tier epoch fallback: DS3231 hardware → NVS Settings → Kconfig bake-in
- Tamper detection: if `|rtc_epoch − nvs_epoch| > 300s`, returns `LIMA_RTC_ERR_SKEW`
- Periodic NVS flush via `k_work_delayable` every 60 seconds
- ISR-safe alarm callback posting `LIMA_EVT_RTC_WAKEUP`
- `k_work` fallback wakeup path when DS3231 is not ready

### `845a8d8` — fix naming conflicts

- Added `src/rtc.c` to `CMakeLists.txt`
- Extended `firmware/Kconfig` with `LIMA_RTC_LOG_LEVEL` and `LIMA_PROVISION_UNIX_TIME` symbols
- Renamed internal functions to avoid conflicts with Zephyr builtins (`rtc_*` → `lima_rtc_*` and `rtc_hw_*`)

### `505068f` — extract rtc functionality from main.c

Removed ~156 lines from `main.c` — the inline RTC init, counter alarm logic, and work callbacks — replaced with the single call:

```c
/* main.c:434 */
void fsm_hw_enter_deep_sleep(void) {
    lima_rtc_arm_wakeup(CONFIG_LIMA_DEEP_SLEEP_INTERVAL_MS);
    hw_ble_stop();
}
```

And in `main()`:

```c
int rtc_ret = lima_rtc_init();
if (rtc_ret == LIMA_RTC_ERR_SKEW) { /* post tamper event */ }
```

---

## Current State of the Code

### What's clean

- `firmware/src/rtc.c` + `rtc.h` — self-contained module, fully documented, compiles cleanly
- `fsm_hw_enter_deep_sleep()` — now a one-liner delegating to `lima_rtc_arm_wakeup()`
- CMakeLists.txt, Kconfig — both updated and correct
- Three-tier epoch fallback with tamper comparison is implemented and tested in logic

### Known Issues and Remaining Work

#### 1. Double `lima_rtc_init()` call in main.c

`main.c` calls `lima_rtc_init()` **twice** — once at line 620 and again at line 639. The second call re-initializes state and reschedules the NVS flush timer redundantly. One of them should be removed.

```c
/* main.c:620 */
int rtc_ret = lima_rtc_init();   /* ← correct call with error handling */
...
/* main.c:639 */
if (lima_rtc_init() != 0) {     /* ← duplicate, should be removed */
    LOG_ERR("[RTC] init failed!");
}
```

#### 2. INT/SQW GPIO wiring still lives in main.c

The DS3231 GPIO interrupt callback and setup are still in `main.c`, not in `rtc.c`:

```c
/* main.c:162–167 */
static struct gpio_callback rtc_gpio_cb_data;
static void rtc_gpio_cb(...) { LOG_INF("[RTC] INT/SQW GPIO fired"); }

/* main.c:633–637 */
gpio_pin_configure_dt(&rtc_int, GPIO_INPUT);
gpio_pin_interrupt_configure_dt(&rtc_int, GPIO_INT_EDGE_FALLING);
gpio_init_callback(&rtc_gpio_cb_data, rtc_gpio_cb, BIT(rtc_int.pin));
gpio_add_callback(rtc_int.port, &rtc_gpio_cb_data);
```

This should move into `lima_rtc_init()` in `rtc.c`. The INT/SQW callback is currently a no-op log line — when real `PM_STATE_SOFT_OFF` is wired, this pin will be the reset source and needs to be handled in `rtc.c`.

#### 3. Sensor thread timestamps still use k_uptime_get_32()

`sensor_thread_fn` posts events with `k_uptime_get_32()` instead of `lima_rtc_timestamp_ms()`. The RTC module exists precisely to fix this — sensor events should carry real wall-clock time:

```c
/* main.c:535 — should be lima_rtc_timestamp_ms() */
.timestamp_ms = k_uptime_get_32(),
```

#### 4. PM_STATE_SOFT_OFF not yet wired

`lima_rtc_arm_wakeup()` arms the DS3231 counter alarm but does not call `pm_state_force()`. Real deep sleep (hardware power-off with GPIO sense wakeup) is still a TODO. The fallback `k_work` path keeps things functional but it is not true low-power sleep.

#### 5. Gateway time-sync path not connected

`lima_rtc_set_epoch()` is fully implemented in `rtc.c` but nothing calls it. The gateway Rust side does not yet send time-sync packets, and the firmware BLE receiver does not parse them.

---

## Suggested Next Steps (in order)

1. **Fix double init** — remove the second `lima_rtc_init()` call at `main.c:639`
2. **Move GPIO wiring into rtc.c** — pull `rtc_int`, `rtc_gpio_cb`, and the four setup lines out of `main.c` and into `lima_rtc_init()`; expose `rtc_int` as a static in `rtc.c`
3. **Update sensor timestamps** — replace `k_uptime_get_32()` in `sensor_thread_fn` with `(uint32_t)lima_rtc_timestamp_ms()`
4. **Wire PM_STATE_SOFT_OFF** — after alarm is armed in `lima_rtc_arm_wakeup()`, call `pm_state_force(0, &(struct pm_state_info){PM_STATE_SOFT_OFF, 0, 0})`; configure INT/SQW pin as a GPIO wakeup source via `nrf_gpio_cfg_sense_set()`
5. **Gateway time-sync** — add a BLE characteristic or ADV-RX command that calls `lima_rtc_set_epoch()` on time packets from the gateway

---

## File Map

```
firmware/
├── CMakeLists.txt              — src/rtc.c registered ✅
├── Kconfig                     — LIMA_RTC_LOG_LEVEL, LIMA_PROVISION_UNIX_TIME ✅
├── prj.conf                    — CONFIG_COUNTER=y, CONFIG_MAXIM_DS3231=y ✅
├── boards/
│   └── nrf52840dk_nrf52840.overlay  — ds3231@68 node (DK only, dongle TBD)
├── src/
│   ├── main.c                  — calls lima_rtc_init() (×2!), GPIO wiring still here
│   ├── rtc.h                   — public API ✅
│   └── rtc.c                   — implementation ✅
└── tools/
    └── provision.py            — sets CONFIG_LIMA_PROVISION_UNIX_TIME at build time

docs/dev/context/
├── context-rtc-swap.md         — DS1307→DS3231 migration reference (pre-refactor)
└── context-rtc-refactor.md     — this file
```
