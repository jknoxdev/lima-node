# sensor_wire — I2C Hardware Validation Test

Ztest suite validating MPU6050 (IMU) and BME280 (barometric) sensor wiring
and driver functionality on the nRF52840-DK (PCA10056).

## Hardware

| Sensor  | Address | Interface | Pins          |
|---------|---------|-----------|---------------|
| MPU6050 | 0x68    | I2C (i2c0)| SDA=P0.05, SCL=P0.04 |
| BME280  | 0x76    | I2C (i2c0)| SDA=P0.05, SCL=P0.04 |

AD0 → GND (MPU6050), CS → VDD, SDO → GND (BME280)

## Test Coverage

- Device readiness (driver init, chip ID validation)
- Single fetch (i2c_write_read round-trip)
- Plausibility ranges (accel magnitude 4.9–19.6 m/s², pressure 700–1100 hPa, temp -10–85°C)
- Read stability (10 consecutive reads, 60ms interval)

## Running
```bash
west build -b nrf52840dk/nrf52840 lima-node/firmware/tests/sensor_wire --pristine
west flash
minicom -D /dev/ttyACM0 -b 115200
```

## Root Cause Note

NCS v3.2.2 / Zephyr v4.2.99 TWI driver does not support chained TX→RX via
NRFX_TWI_FLAG_SUSPEND — nrfx_twi explicitly rejects RX after TX suspend.
TWIM EasyDMA handles write-read atomically. Overlay must specify
`compatible = "nordic,nrf-twim"` and use P0.04/P0.05 (Arduino header defaults).

See [docs](docs) for logic analyzer traces documenting the debugging process.