# macos

```bash
west build -b nrf52840_mdk_usb_dongle lima-node/firmware --pristine  -- -DCONFIG_BUILD_OUTPUT_UF2=y
```
```bash
cp build/firmware/zephyr/zephyr.uf2 /Volumes/UF2BOOT      
```
```bash
screen /dev/tty.usbmodem
```

# linux 

```bash 
# fresh build after machine switching ie. mac->linux
west build -b nrf52840_mdk_usb_dongle lima-node/firmware --pristine -- -DCONFIG_BUILD_OUTPUT_UF2=y
```
```bash
cp build/firmware/zephyr/zephyr.uf2 /media/$USER/UF2BOOT 
```
```bash
screen /dev/ttyACM0
```

## search for overlay files
```bash
find . -name "*.overlay" 2>/dev/null
```