# LIMA — RTC Swap Context: DS1307 → DS3231

> **Created:** 2026-03-21
> **Branch:** `main`
> **Purpose:** Task-specific context for replacing the DS1307 with the DS3231 that arrived today.

---

## Current State of RTC in Firmware

The RTC wakeup is a **software stub** — no hardware RTC is currently instantiated in the firmware. The stub lives in `firmware/src/main.c`:

```c
/* main.c:87 */
static struct k_work_delayable rtc_wakeup_work;

/* main.c:149-158 */
static void rtc_wakeup_expiry_fn(struct k_work *work)
{
    ARG_UNUSED(work);
    LOG_INF("[RTC] wakeup timer fired — posting LIMA_EVT_RTC_WAKEUP");
    lima_event_t e = {
        .type         = LIMA_EVT_RTC_WAKEUP,
        .timestamp_ms = k_uptime_get_32(),
    };
    lima_post_event(&e);
}
```

The stub is armed in `fsm_hw_enter_deep_sleep()` (which lives in `main.c` as an FSM HAL trampoline, called by `fsm.c`):

```c
/* main.c ~line 425 */
void fsm_hw_enter_deep_sleep(void)
{
    /* Stub RTC wakeup — real PM_STATE_SOFT_OFF replaces this in v2 */
    k_work_reschedule(&rtc_wakeup_work,
                      K_MSEC(CONFIG_LIMA_DEEP_SLEEP_INTERVAL_MS));
}
```

The deep sleep state in `fsm.c` (lines ~321-349) waits for `LIMA_EVT_RTC_WAKEUP` to transition back to `ARMED`.

**Real `PM_STATE_SOFT_OFF` is not wired.** Sleep is a software delay, not a hardware power-off.

---

## Why DS3231 (Not DS1307)

| Feature | DS1307 | DS3231 |
|---|---|---|
| Crystal | External, uncompensated | Internal TCXO (±2 ppm) |
| Alarm outputs | **None** | 2 alarms via INT/SQW pin |
| Zephyr driver | None upstream | `maxim,ds3231` — upstream in Zephyr |
| I2C address | 0x68 | 0x68 |
| Wakeup capability | Not without external circuitry | Yes — INT/SQW pulls low on alarm |

The DS3231 alarm output is what enables interrupt-driven wakeup from `PM_STATE_SOFT_OFF`. Without it the DS1307 would have required external circuitry. DS3231 is a strict upgrade.

---

## Zephyr DS3231 Driver

The Zephyr NCS tree already has the DS3231 driver. Proof — generated syscall header exists in the current build artifacts:

```
firmware/build/firmware/zephyr/include/generated/zephyr/syscalls/maxim_ds3231.h
```

The driver uses Zephyr's **counter subsystem** (not the newer `rtc` API). Key APIs:

```c
#include <zephyr/drivers/counter.h>
/* DS3231-specific syncpoint API: */
#include <zephyr/drivers/rtc/maxim_ds3231.h>

const struct device *rtc = DEVICE_DT_GET(DT_NODELABEL(ds3231));

/* Arm a one-shot alarm N seconds from now: */
struct counter_alarm_cfg alarm = {
    .callback = rtc_alarm_cb,
    .ticks    = counter_us_to_ticks(rtc, interval_us),
    .user_data = NULL,
    .flags    = 0,
};
counter_set_channel_alarm(rtc, 0, &alarm);
counter_start(rtc);
```

The alarm callback fires from ISR context — post the event via `k_work` or the message queue with `K_NO_WAIT` from within it.

---

## I2C Address Conflict Check

DS3231 I2C address is fixed at **0x68**.

| Board | MPU6050 addr | DS3231 addr | Conflict? |
|---|---|---|---|
| `nrf52840_mdk_usb_dongle` | **0x69** (AD0 pulled high on dongle PCB) | 0x68 | **No conflict** ✅ |
| `nrf52840dk_nrf52840` | **0x68** (AD0 default low) | 0x68 | **CONFLICT** ⚠️ |

Primary target is the dongle board — no conflict. The DK overlay (`nrf52840dk_nrf52840.overlay`) should not get the DS3231 node until MPU6050 AD0 is pulled high on that board.

---

## Files to Change

### 1. `firmware/boards/nrf52840_mdk_usb_dongle.overlay`

Add DS3231 node to the existing `&i2c0` block. Need to pick a free GPIO pin for `isw-gpios` (INT/SQW alarm output, active-low):

```dts
ds3231: ds3231@68 {
    compatible = "maxim,ds3231";
    reg = <0x68>;
    isw-gpios = <&gpio0 XX GPIO_ACTIVE_LOW>;  /* ← pick free pin */
    status = "okay";
};
```

The dongle has limited exposed GPIOs — check the MDK USB Dongle pinout in `artifacts/` to select a free pin.

### 2. `firmware/prj.conf`

Add:
```
CONFIG_COUNTER=y
CONFIG_MAXIM_DS3231=y
CONFIG_MAXIM_DS3231_SHELL=n
```

### 3. `firmware/src/main.c`

- Replace `static struct k_work_delayable rtc_wakeup_work` with DS3231 device binding
- Replace `rtc_wakeup_expiry_fn` (work callback) with a counter alarm callback
- Replace `k_work_reschedule` in `fsm_hw_enter_deep_sleep()` with `counter_set_channel_alarm()` + `counter_start()`
- Optionally: call `pm_state_force(0, &(struct pm_state_info){PM_STATE_SOFT_OFF, 0, 0})` after arming alarm

### 4. `firmware/src/fsm.c`

- `fsm_hw_enter_deep_sleep()` is declared in `fsm.h` and implemented in `main.c` (HAL trampoline pattern)
- No changes needed in `fsm.c` itself — it only calls the trampoline and waits for `LIMA_EVT_RTC_WAKEUP`

---

## Current Device Tree (Dongle Overlay, Unchanged)

```dts
/* firmware/boards/nrf52840_mdk_usb_dongle.overlay */
&i2c0 {
    compatible = "nordic,nrf-twim";
    /delete-property/ zephyr,pm-device-runtime-auto;
    status = "okay";
    pinctrl-0 = <&i2c0_default>;
    pinctrl-1 = <&i2c0_sleep>;
    pinctrl-names = "default", "sleep";
    clock-frequency = <I2C_BITRATE_STANDARD>;

    mpu6050: mpu6050@69 {
        compatible = "invensense,mpu6050";
        reg = <0x69>;
        status = "okay";
        accel-fs = <2>;
        gyro-fs = <250>;
        smplrt-div = <0>;
    };

    bme280: bme280@76 {
        compatible = "bosch,bme280";
        reg = <0x76>;
        status = "okay";
    };
};
```

DS3231 node goes inside this same `&i2c0` block.

---

## Relevant prj.conf (Current, Unchanged)

```
CONFIG_I2C=y
CONFIG_I2C_LOG_LEVEL_DBG=y
CONFIG_SENSOR=y
CONFIG_MPU6050=y
CONFIG_BME280=y
CONFIG_LIMA_DEEP_SLEEP_INTERVAL_MS=6000
CONFIG_PM_DEVICE=y
```

`CONFIG_LIMA_DEEP_SLEEP_INTERVAL_MS=6000` (6 seconds) will become the DS3231 alarm interval in ms once wired.

---

## Known Gotchas

1. **Counter ticks ≠ milliseconds** — `counter_us_to_ticks()` converts microseconds. Pass `CONFIG_LIMA_DEEP_SLEEP_INTERVAL_MS * 1000ULL` as the microsecond value.

2. **ISR context in alarm callback** — DS3231 counter alarm callbacks fire from ISR. Use `k_msgq_put(&fsm_msgq, &e, K_NO_WAIT)` directly (it is ISR-safe) rather than `lima_post_event()` if that function is not ISR-safe. Verify.

3. **PM_STATE_SOFT_OFF and thread wakeup** — when real soft-off is enabled, the system resets on wakeup (GPIO sense → reset). The INT/SQW pin needs to be configured as a wake source via `NRF_GPIOTE` or `nrf_gpio_cfg_sense_set()` before entering soft-off. This is separate from the counter alarm callback path.

4. **DS3231 needs initial time set** — on first boot the DS3231 oscillator may not be running (OSF bit set). Call `maxim_ds3231_req_syncpoint()` to sync system time to RTC, or at minimum verify the oscillator is running before arming alarm.

5. **`sensor_thread` still runs during stub sleep** — the 60ms poll thread skips reads via `fsm_get_state()` check, but doesn't suspend. With real `PM_STATE_SOFT_OFF` the system halts entirely, so this resolves itself.
