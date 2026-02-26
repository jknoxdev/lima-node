# GY-521 / MPU-6050 Pinout Reference

> **Module:** GY-521 breakout board (generic)  
> **Chip:** InvenSense MPU-6050  
> **Interface:** I2C  
> **Target:** nRF52840-MDK-USB-Dongle / Zephyr RTOS  

---

## Pin Connections

| GY-521 Pin | Connect To | Notes |
|---|---|---|
| VCC | 3.3V | **Do not use 5V** — connect to 3.3V, bypasses regulator |
| GND | GND | |
| SCL | nRF52840 SCL | I2C clock |
| SDA | nRF52840 SDA | I2C bidirectional  |
| INT | nRF52840 GPIO | Interrupt low on m-threshold, ARMED → EVENT_DETECTED |
| AD0 | GND | I2C address select — GND = 0x68 (default), 3.3V = 0x69 |
| XDA | unused in mvp | Aux Data — secondary I2C bus for external magnetometer |
| XCL | unused in mvp  | Aux Clock — pairs with XDA |

---

## I2C Address

| AD0 | Address |
|---|---|
| GND (default) | `0x68` |
| 3.3V | `0x69` |

---

- [MPU-6050 Product Specification Rev 3.4](https://invensense.tdk.com/wp-content/uploads/2015/02/MPU-6000-Datasheet1.pdf)
- [MPU-6050 Register Map and Descriptions](https://invensense.tdk.com/wp-content/uploads/2015/02/MPU-6000-Register-Map1.pdf)

---

*Part of the LIMA Node hardware reference — see `artifacts/specsheets/` for full datasheets.*
