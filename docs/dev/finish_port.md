# finish_port.md — I2C Port Checklist (sensor_wire → main firmware)

Validated on nRF52840-DK via `firmware/tests/sensor_wire`.
Target: `nrf52840_mdk_usb_dongle` (production board).

---

## Critical — Will Break Without These

### 1. Add `compatible = "nordic,nrf-twim"` to the dongle overlay

**Test overlay** (`nrf52840dk_nrf52840.overlay:1`):
```dts
&i2c0 {
    compatible = "nordic,nrf-twim";
    ...
}
```

**Main overlay** (`nrf52840_mdk_usb_dongle.overlay:1`): **missing**.

NCS v3.2.2 / Zephyr v4.2.99 TWI driver rejects chained TX→RX via
`NRFX_TWI_FLAG_SUSPEND`. TWIM EasyDMA handles write-read atomically.
Without this, BME280 calibration burst reads and MPU6050 WHO_AM_I reads fail.

---

### 2. Fix the I2C recovery pin mismatch in `main.c`

`main.c:43-44` hardcodes DK pins:
```c
#define I2C0_SCL_PIN 04
#define I2C0_SDA_PIN 05
```

But the dongle overlay (`nrf52840_mdk_usb_dongle.overlay:19-20`) uses:
```dts
psels = <NRF_PSEL(TWIM_SCL, 0, 19)>,
        <NRF_PSEL(TWIM_SDA, 0, 20)>;
```

`hw_i2c_bus_recovery()` (`main.c:258`) will toggle the wrong pins on the dongle.
Update the defines to match the actual dongle overlay pins (P0.19/P0.20).

---

### 3. Add `status = "okay"` and node labels to sensor nodes in dongle overlay

**Test overlay** has labeled nodes with status:
```dts
mpu6050: mpu6050@68 {
    compatible = "invensense,mpu6050";
    reg = <0x68>;
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
```

**Main overlay** has neither labels nor `status = "okay"`:
```dts
mpu6050@68 {
    compatible = "invensense,mpu6050";
    reg = <0x68>;
    int-gpios = <&gpio0 8 GPIO_ACTIVE_HIGH>;
};
```

Labels are required if `hw_init_sensors()` is changed to use
`DEVICE_DT_GET(DT_NODELABEL(...))` (see item 6 below).

---

### 4. Verify BME280 I2C address (0x76 vs 0x77)

- Test overlay + sensor_wire README: `bme280@76` (SDO → GND)
- Main overlay: `bme280@77` (SDO → VDD)

Check the SDO/ADDR pin wiring on your dongle breakout. If SDO is tied to GND,
change the dongle overlay to `bme280@76`.  If VDD, keep `bme280@77`.

Mismatched address = silent NACK; `device_is_ready()` returns false.

---

## Important — May Cause Intermittent Failures

### 5. Delete `zephyr,pm-device-runtime-auto` and set clock-frequency explicitly

Test overlay:
```dts
&i2c0 {
    compatible = "nordic,nrf-twim";
    /delete-property/ zephyr,pm-device-runtime-auto;
    clock-frequency = <I2C_BITRATE_STANDARD>;
    ...
}
```

Main overlay is missing both. `zephyr,pm-device-runtime-auto` can power-gate
the TWIM peripheral between transactions causing intermittent NACK on first
transfer after idle. The explicit 100 kHz clock avoids inheriting a board
default that may differ.

---

### 6. Add `CONFIG_I2C_NRFX_TRANSFER_TIMEOUT` and disable PM device runtime

Add to `firmware/prj.conf`:
```
CONFIG_I2C_NRFX_TRANSFER_TIMEOUT=2000
CONFIG_PM_DEVICE_RUNTIME=n
```

`CONFIG_PM_DEVICE=y` is already present. The test explicitly sets
`CONFIG_PM_DEVICE_RUNTIME=n` to prevent the driver from suspending
the bus between sensor reads. Without the timeout override, a stalled
bus can hang the I2C thread indefinitely.

---

## Nice to Have — Robustness Improvements

### 7. Switch sensor init to `DEVICE_DT_GET(DT_NODELABEL(...))`

`main.c:163,175` uses:
```c
mpu = DEVICE_DT_GET_ANY(invensense_mpu6050);
bme = DEVICE_DT_GET_ANY(bosch_bme280);
```

The test uses:
```c
static const struct device *mpu = DEVICE_DT_GET(DT_NODELABEL(mpu6050));
static const struct device *bme = DEVICE_DT_GET(DT_NODELABEL(bme280));
```

`DEVICE_DT_GET_ANY` returns the first matching compatible in the tree, which
is fine for one board but fragile if the dongle DTS ever gains a second device
with the same compatible. Once overlay labels are added (item 3), prefer the
labeled form for explicit binding.

---

### 8. Port MPU6050 driver properties to dongle overlay

Test overlay sets:
```dts
accel-fs = <2>;     /* ±2g full-scale */
gyro-fs = <250>;    /* ±250°/s */
smplrt-div = <0>;   /* max sample rate */
```

Main overlay omits these (driver uses defaults). Add them to match the
validated sensor configuration and ensure consistent sensitivity.

---

### 9. Keep `int-gpios` or document its status

Main overlay has:
```dts
int-gpios = <&gpio0 8 GPIO_ACTIVE_HIGH>;
```

The test overlay does not use this. Motion interrupt is not currently wired
in `main.c` (the FSM polls via `sensor_sample_fetch`). Either:
- Keep it for future interrupt-driven motion wake (document the GPIO reservation)
- Remove it if P0.08 is not physically connected on the dongle build

---

## Summary Table

| # | File | Change | Priority |
|---|------|--------|----------|
| 1 | dongle overlay | Add `compatible = "nordic,nrf-twim"` to `&i2c0` | Critical |
| 2 | `main.c` | Fix `I2C0_SCL_PIN`/`I2C0_SDA_PIN` to match dongle overlay pins | Critical |
| 3 | dongle overlay | Add labels + `status = "okay"` to sensor nodes | Critical |
| 4 | dongle overlay | Verify `bme280@76` vs `@77` against hardware SDO pin | Critical |
| 5 | dongle overlay | Add `/delete-property/ zephyr,pm-device-runtime-auto` + `clock-frequency` | Important |
| 6 | `prj.conf` | Add `CONFIG_I2C_NRFX_TRANSFER_TIMEOUT=2000` + `CONFIG_PM_DEVICE_RUNTIME=n` | Important |
| 7 | `main.c` | Switch to `DEVICE_DT_GET(DT_NODELABEL(...))` after labels added | Nice |
| 8 | dongle overlay | Add `accel-fs`, `gyro-fs`, `smplrt-div` to MPU6050 node | Nice |
| 9 | dongle overlay | Decide on `int-gpios` — keep or remove | Nice |
