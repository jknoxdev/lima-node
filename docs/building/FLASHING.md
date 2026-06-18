# Flashing Guide

> **Tested against:** NCS v3.2.0-rc1 · Zephyr 4.3.99 · Board: `nrf52840_mdk_usb_dongle/nrf52840`  
> If your NCS version differs, commands may need adjustment. Check the version header first.

---

## Prerequisites

One-time setup — see [`docs/DEV_SETUP.md`](DEV_SETUP.md) for full environment setup.

```bash
# Activate workspace
cd ~/your-workspace
source .venv/bin/activate
```

## One-Time Board Fix

The MDK USB Dongle uses UF2, not J-Link. Patch the board runner once:

```bash
nano zephyr/boards/makerdiary/nrf52840_mdk_usb_dongle/board.cmake
```

Change the runner line to:
```cmake
include(${ZEPHYR_BASE}/boards/common/uf2.board.cmake)
```

Also enable Read/Write permissions in your repo:  
**GitHub → Settings → Actions → General → Workflow permissions → Read and write**

---

## Build

```bash
west build -b nrf52840_mdk_usb_dongle/nrf52840 lima-node/firmware \
  -- -DCONFIG_BUILD_OUTPUT_UF2=y
```

Clean build:
```bash
west build -p always -b nrf52840_mdk_usb_dongle/nrf52840 lima-node/firmware \
  -- -DCONFIG_BUILD_OUTPUT_UF2=y
```

---

## Flash

1. Put dongle in bootloader mode — **hold RESET while plugging in**
2. RGB LED pulses **red** → bootloader active, mounts as `UF2BOOT`
3. Flash:

```bash
west flash
```

---

## Verify

Open a serial monitor to confirm firmware is running:

```bash
screen /dev/ttyACM0 115200
# or
minicom -D /dev/ttyACM0 -b 115200
```

---

## Troubleshooting

| Error | Fix |
|---|---|
| `runner not configured` | board.cmake not patched — see One-Time Board Fix above |
| `uf2 file location unknown` | Missing `-DCONFIG_BUILD_OUTPUT_UF2=y` in build command |
| `build.ninja not found` | Stale build dir — use `-p always` flag |
| Dongle not mounting as UF2BOOT | Not in bootloader mode — hold RESET while plugging in |
| `west flash` finds no device | Check `ls /dev/tty*` — may be `ttyACM1` not `ttyACM0` |

---

*For full background on this setup including NCS workspace initialization and toolchain install, see the [complete flash howto](lima-node-flash-howto.docx).*
