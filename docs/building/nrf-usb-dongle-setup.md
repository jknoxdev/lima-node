# build notes


# 🚀 nRF52840 MDK USB Dongle: Build & Flash Notes


## 1. Build from workspace
Run this from your `ncs-playground` root. Using `-p always` ensures a clean (pristine) build every time.
```bash
# expample load venv
python3 -m venv ~/apps/nrf52840/my-workspace/.venv/bin/activate

# build in ~/apps/nrf52840/my-workspace/
west build -p always -b nrf52840_mdk_usb_dongle samples/blinky
```
![alt text](image.png)

## 2. Convert HEX to UF2
Zephyr outputs a .hex file. Your bootloader requires a .uf2 file.

Note: The build artifacts are located in build/zephyr/, not build/blinky/.

-f 0xada52840: The specific Family ID for nRF52840.

```
uf2conv build/blinky/zephyr/zephyr.hex -c -f 0xada52840 -o ./zephyr.uf2
```
![alt text](image-2.png)



