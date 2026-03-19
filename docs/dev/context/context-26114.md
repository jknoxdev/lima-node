# LIMA Project — Context Snapshot 2026-03-12

> **Last updated:** 2026-03-12
> **Branch:** `feature/dk-port`
> **Purpose:** Post-DK-port state snapshot. Focus: sensor wire verification tests.

---

## What Just Happened

The codebase was ported from the **nRF52840 MDK USB Dongle** to the **nRF52840 DK** development board.
Two commits landed on `feature/dk-port`:

| Commit | Change |
|--------|--------|
| `3ee647f` | Add `nrf52840dk_nrf52840.overlay` with DK pin mapping |
| `fc1b965` | Fix: add `UART_RX P0.8` to uart0 pinctrl groups (was missing) |

The overlay is the **only** hardware delta. All firmware logic (`main.c`, `fsm.c`, `crypto.c`, `ble.c`) is unchanged.

---

## DK Pin Mapping vs Dongle

| Signal | MDK USB Dongle | nRF52840 DK |
|--------|---------------|-------------|
| I2C0 SCL | P0.19 | **P0.4** |
| I2C0 SDA | P0.20 | **P0.5** |
| MPU6050 INT | P0.8 | **P0.3** |
| UART TX | P0.6 | P0.6 (same) |
| UART RX | (not present) | **P0.8** |
| LED Red | P0.23 | **P0.13** |
| LED Green | P0.22 | **P0.14** |
| LED Blue | P0.24 | **P0.15** |

Note: `main.c` still has `#define I2C0_SCL_PIN 04` / `#define I2C0_SDA_PIN 05` — these are
the DK values and match the overlay. The dongle values (19, 20) are commented out directly above.

---

## Sensor Quick Reference

### MPU6050 (IMU)
- I2C address: `0x68`
- INT pin: `P0.3` (DK) — configured as `int-gpios` in overlay, also aliased as `mpu-int`
- Threshold: `MOTION_THRESHOLD_G = 0.80g`
- Reading: `sensor_channel_get(mpu, SENSOR_CHAN_ACCEL_XYZ, accel)`, then `sqrt(ax²+ay²+az²)`
- Fires: `LIMA_EVT_MOTION_DETECTED`

### BME280 (Barometric)
- I2C address: `0x77`
- Same I2C bus as MPU6050
- Threshold: `CONFIG_LIMA_BARO_THRESHOLD_PA = 5 Pa`
- Baseline tracking: exponential moving average, alpha = 0.01 (slow drift compensation)
- Fires: `LIMA_EVT_PRESSURE_BREACH`

---

## Next Goal: Sensor Wire Verification Tests

After a board port, the highest risk is silent wiring failures — a sensor that initializes
but returns garbage, or silently fails `sensor_sample_fetch()` with `-EIO` because a pin is
wrong or floating. The existing firmware already has log output that catches gross failures,
but we need **repeatable, pass/fail tests** to confirm wire with confidence.

### Recommended Test Approach: Zephyr Ztest on-target

Zephyr has a first-class test framework (`ztest`) that can run directly on the DK over serial.
The test app lives in its own CMake target (`tests/`) alongside the main firmware.

---

## Test Plan: `firmware/tests/sensor_wire/`

Create a dedicated Ztest app at `firmware/tests/sensor_wire/` that builds separately from the
main firmware and flashes to the DK to confirm sensor wiring.

### File layout

```
firmware/tests/sensor_wire/
├── CMakeLists.txt
├── prj.conf
├── boards/
│   └── nrf52840dk_nrf52840.overlay   # symlink or copy from firmware/boards/
└── src/
    └── main.c
```

### `prj.conf` (minimal)

```ini
CONFIG_ZTEST=y
CONFIG_ZTEST_NEW_API=y
CONFIG_I2C=y
CONFIG_SENSOR=y
CONFIG_MPU6050=y
CONFIG_BME280=y
CONFIG_LOG=y
CONFIG_LOG_DEFAULT_LEVEL=3
CONFIG_FPU=y
CONFIG_NEWLIB_LIBC=y   # needed for sqrtf
```

### Test cases to implement in `src/main.c`

```c
#include <zephyr/ztest.h>
#include <zephyr/drivers/sensor.h>
#include <math.h>

/* ── Fixtures ───────────────────────────────────────────────────────────── */

static const struct device *mpu;
static const struct device *bme;

static void *sensor_suite_setup(void)
{
    mpu = DEVICE_DT_GET(DT_NODELABEL(mpu6050));   /* alias from overlay */
    bme = DEVICE_DT_GET(DT_NODELABEL(bme280));
    return NULL;
}

ZTEST_SUITE(sensor_wire, NULL, sensor_suite_setup, NULL, NULL, NULL);

/* ── Test 1: Device ready ────────────────────────────────────────────────── */

ZTEST(sensor_wire, test_mpu6050_ready)
{
    zassert_true(device_is_ready(mpu),
        "MPU6050 not ready — check I2C wiring P0.4/P0.5 and address 0x68");
}

ZTEST(sensor_wire, test_bme280_ready)
{
    zassert_true(device_is_ready(bme),
        "BME280 not ready — check I2C wiring P0.4/P0.5 and address 0x77");
}

/* ── Test 2: Fetch succeeds (no -EIO / bus error) ────────────────────────── */

ZTEST(sensor_wire, test_mpu6050_fetch)
{
    int rc = sensor_sample_fetch(mpu);
    zassert_ok(rc, "MPU6050 fetch failed (%d) — I2C comms broken or wrong pin", rc);
}

ZTEST(sensor_wire, test_bme280_fetch)
{
    int rc = sensor_sample_fetch(bme);
    zassert_ok(rc, "BME280 fetch failed (%d) — I2C comms broken or wrong pin", rc);
}

/* ── Test 3: Values in plausible physical range ──────────────────────────── */

ZTEST(sensor_wire, test_mpu6050_accel_range)
{
    struct sensor_value ax, ay, az;
    zassert_ok(sensor_sample_fetch(mpu), "fetch failed");
    zassert_ok(sensor_channel_get(mpu, SENSOR_CHAN_ACCEL_X, &ax), "get X failed");
    zassert_ok(sensor_channel_get(mpu, SENSOR_CHAN_ACCEL_Y, &ay), "get Y failed");
    zassert_ok(sensor_channel_get(mpu, SENSOR_CHAN_ACCEL_Z, &az), "get Z failed");

    float x = sensor_value_to_float(&ax);
    float y = sensor_value_to_float(&ay);
    float z = sensor_value_to_float(&az);
    float mag = sqrtf(x*x + y*y + z*z);

    /* Stationary on desk: magnitude should be ~9.8 m/s² (1g).
       Accept 0.5g–2.0g to cover tilted placement. */
    zassert_true(mag > 4.9f && mag < 19.6f,
        "MPU6050 accel magnitude %.2f m/s² out of range — sensor may be returning zeros or garbage", mag);
}

ZTEST(sensor_wire, test_bme280_pressure_range)
{
    struct sensor_value press;
    zassert_ok(sensor_sample_fetch(bme), "fetch failed");
    zassert_ok(sensor_channel_get(bme, SENSOR_CHAN_PRESS, &press), "get pressure failed");

    /* Sea-level ±300 hPa covers any realistic altitude.
       BME280 returns kPa — multiply by 10 for hPa. */
    float hpa = sensor_value_to_float(&press) * 10.0f;
    zassert_true(hpa > 700.0f && hpa < 1100.0f,
        "BME280 pressure %.1f hPa out of range — sensor may be returning zeros or all-ones", hpa);
}

ZTEST(sensor_wire, test_bme280_temperature_range)
{
    struct sensor_value temp;
    zassert_ok(sensor_sample_fetch(bme), "fetch failed");
    zassert_ok(sensor_channel_get(bme, SENSOR_CHAN_AMBIENT_TEMP, &temp), "get temp failed");

    float c = sensor_value_to_float(&temp);
    /* Room temperature: accept -10°C to 85°C */
    zassert_true(c > -10.0f && c < 85.0f,
        "BME280 temperature %.1f°C out of range — check sensor", c);
}

/* ── Test 4: Repeated reads are stable (no -EIO after first read) ─────────── */

ZTEST(sensor_wire, test_mpu6050_read_stability)
{
    for (int i = 0; i < 10; i++) {
        int rc = sensor_sample_fetch(mpu);
        zassert_ok(rc, "MPU6050 read %d/10 failed (%d) — intermittent I2C issue", i+1, rc);
        k_msleep(60); /* match production poll rate */
    }
}

ZTEST(sensor_wire, test_bme280_read_stability)
{
    for (int i = 0; i < 10; i++) {
        int rc = sensor_sample_fetch(bme);
        zassert_ok(rc, "BME280 read %d/10 failed (%d) — intermittent I2C issue", i+1, rc);
        k_msleep(60);
    }
}

/* ── Test 5: I2C bus recovery ────────────────────────────────────────────── */
/* This test just confirms the recovery path doesn't crash.
   In production main.c, hw_i2c_bus_recovery() is called on -EIO.
   A full test of recovery would require injecting a bus fault — skip for now. */
```

### Build & run commands

```bash
# Build the test app for the DK
west build -b nrf52840dk_nrf52840 firmware/tests/sensor_wire --pristine

# Flash (DK connected via USB debugger — no bootloader mode needed)
west flash

# Monitor test output
minicom -D /dev/ttyACM0 -C sensor-wire-$(date +%H%M%S).cap

# Or with west
west build -t flash && minicom -D /dev/ttyACM0
```

### Expected pass output

```
Running TESTSUITE sensor_wire
===================================================================
START - test_mpu6050_ready
 PASS - test_mpu6050_ready in 0ms
START - test_bme280_ready
 PASS - test_bme280_ready in 0ms
START - test_mpu6050_fetch
 PASS - test_mpu6050_fetch in 2ms
START - test_bme280_fetch
 PASS - test_bme280_fetch in 4ms
START - test_mpu6050_accel_range
 PASS - test_mpu6050_accel_range in 3ms
START - test_bme280_pressure_range
 PASS - test_bme280_pressure_range in 5ms
START - test_bme280_temperature_range
 PASS - test_bme280_temperature_range in 1ms
START - test_mpu6050_read_stability
 PASS - test_mpu6050_read_stability in 640ms
START - test_bme280_read_stability
 PASS - test_bme280_read_stability in 640ms
===================================================================
TESTSUITE sensor_wire succeeded
```

### Failure triage guide

| Failure | Likely cause |
|---------|-------------|
| `test_mpu6050_ready FAIL` | MPU6050 not on bus — wrong SCL/SDA, no power, wrong I2C address |
| `test_bme280_ready FAIL` | BME280 not on bus — check SDO pin (pulls addr to 0x76 vs 0x77) |
| `test_*_fetch FAIL (-5)` | I2C bus error (-EIO) — check pull-ups, wire length, signal quality |
| `test_mpu6050_accel_range FAIL` (mag ≈ 0) | Sensor returning zeros — DMP/FIFO mode issue or I2C partial comms |
| `test_bme280_pressure_range FAIL` (hPa ≈ 0) | BME280 in reset or not responding to config |
| `test_*_stability FAIL` on read 3+ | Loose wire or marginal pull-up causing intermittent bus hangs |

---

## Known Issues Carried Over from Dongle Build

1. **`lima_crypto_sign_async()` is synchronous** — blocks FSM thread ~107ms. Name is aspirational.
2. **`node_id` hardcoded** as `0xDE:AD:BE:EF:00:01` — needs `bt_id_get()`.
3. **Sequence counter resets on boot** — NVS persistence TODO.
4. **Sensor thread polls in DEEP_SLEEP** — checks FSM state and skips reads, but thread still wakes every 60ms.
5. **Watchdog disabled** (`wdt_disable(wdt)` in `main()`) — intentional for dev.
6. **Public key export error** visible in crypto log: `CRYPTO: public key export failed (-134)` — this is a key attribute/usage flags issue with Oberon PSA driver. Signing still works. Track separately.

---

## Build Quick Reference (DK)

```bash
# Build main firmware for DK
west build -b nrf52840dk_nrf52840 lima-node/firmware --pristine

# Flash via JLink (DK onboard debugger — no bootloader mode needed)
west flash

# Serial console (DK always has a stable ACM port)
minicom -D /dev/ttyACM0 -C debug-$(date +%H%M%S).cap
```

---

## Next Session Starting Points

### Option A — Implement the sensor wire tests (highest priority)
Create `firmware/tests/sensor_wire/` per the test plan above. Flash, run, capture output.
Goal: confirm both sensors are live on DK before any further firmware work.

### Option B — Merge DK port to main
Once sensor wire tests pass, open a PR from `feature/dk-port` into `main`.
The overlay commit is clean; the only question is whether the I2C pin defines in `main.c`
(`I2C0_SCL_PIN 04`, `I2C0_SDA_PIN 05`) should be made board-conditional or left as-is
with dongle values commented out.

### Option C — Resume gateway MQTT work
If DK port is confirmed good and tests pass, the gateway MQTT publisher is the next active
development front (per previous context.md). `rumqttc` is already in `Cargo.toml`.

---

## Recent Git History (feature/dk-port)

```
fc1b965  fix(dk-port): add UART_RX P0.8 to uart0 pinctrl groups
3ee647f  feat(dk-port): add nRF52840 DK overlay with DK pin mapping
d87a291  chore(git): ignore firmware build directories
9930a20  feat(gateway): BLE scanner + crypto verification + ratatui TUI skeleton (#7)
1f192e6  docs: update README — BME280, ADR-006 resolved, context.md, roadmap (#5)
```
