# Hardware Initialization
This directory contains the UF2 Bootloader required to modernize the 2019-era MakerDiary nRF52840 MDK Dongles.

# Version: 1.2.0 (Adafruit-nRF52 Based)
Flash Method: > adafruit-nrfutil dfu serial -pkg nrf52840_mdk_usb_dongle_v1.2.0.zip -p /dev/cu.usbmodemXXXX -b 115200

<35;98;20M./debugprobe.uf2 : used for the rp2040 picoprobe to bootstrap and force load the nrf firmware

```
brew install openocd
```

| RP2040-Zero | nRF52840 Dongle | Function |
| :--- | :--- | :--- |
| GP2 | Pin 17 | SWDCLK |
| GP3 | Pin 16 | SWDIO |
| GND | Pin 19 | Ground |
```
```

```
openocd -f interface/cmsis-dap.cfg -f target/nrf52.cfg -c "adapter speed 1000; init; halt; nrf5 mass_erase; shutdown"
```

```
openocd -f interface/cmsis-dap.cfg -f target/nrf52.cfg -c "init; halt; program uf2_bootloader-nrf52840_mdk_usb_dongle-0.7.1-s140_6.1.1.hex verify reset; shutdown"
```
```

