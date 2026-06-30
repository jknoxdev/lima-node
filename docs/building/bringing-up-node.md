## Bringing Up the L.I.M.A. Node
This project uses a Local Manifest topology. Follow these steps to initialize the workspace and install the necessary dependencies for the nRF52840.

### 1. Prerequisite: Python Environment

We recommend using a virtual environment to avoid dependency drift.

> ⚠️ macOS Tahoe 26.5.1 ships with Python 3.14 by default — Zephyr/NCS tooling isn't tested against it yet and several dependencies (`patool`, `click`, etc.) will fail to resolve. Use 3.12:
> ```bash
> brew install python@3.12
> ```

```bash
cd ~/lima-ws
/opt/homebrew/bin/python3.12 -m venv .venv
source .venv/bin/activate
```

> If you already created a venv with the system default Python (3.14) and are hitting `ERROR: Could not find a version that satisfies the requirement...`, blow it away and recreate against 3.12:
> ```bash
> deactivate
> rm -rf .venv
> /opt/homebrew/bin/python3.12 -m venv .venv
> source .venv/bin/activate
> ```

### Install West (Zephyr's meta-tool)
```bash
pip install west
```

### 2. Initialize the Workspace
The `west.yml` in this repository acts as the master blueprint for the entire SDK.

```bash
# Initialize the workspace using this repo as the local manifest
west init -l lima-node

# Pull the Nordic Connect SDK (NCS), Zephyr, and HAL modules
west update
```

### 3.a Install SDK Requirements
Once the modules are downloaded, install the specific toolchain requirements:

```bash
pip install -r zephyr/scripts/requirements.txt
pip install -r nrf/scripts/requirements.txt
pip install -r bootloader/mcuboot/scripts/requirements.txt
```

> Module folder name (`nrf/` vs `sdk-nrf/`) can vary by NCS version — confirm with `ls` after `west update` if `pip install -r nrf/...` 404s.

### 3.b Install the sdk
```bash
west sdk install
```
> ⚠️ On macOS you'll see `SKIPPED: macOS host tools are not available yet.` — this is expected, the Zephyr SDK's host tools bundle (QEMU, OpenOCD, etc.) isn't packaged for macOS. The cross-compiler toolchain itself still installs fine. Flashing the DK uses its onboard J-Link, not these host tools — see Step 3.c / `FLASHING.md`.


### 3.c Install LIMA-Node tools: 
```bash
brew install tio
```

### 4. Build & Verify
Test the toolchain by building the firmware for the nRF52840 DK:

```bash
west build -b nrf52840dk/nrf52840 lima-node/firmware -p always
```
> The `-p always` (pristine) flag forces a clean reconfigure. If you hit `ninja: error: loading 'build.ninja': No such file or directory`, it means a previous build attempt failed before CMake finished configuring — this flag fixes it.

Expected output — memory report + successful link:
```bash
Memory region    Used Size   Region Size   % Used
FLASH:           ...
RAM:             ...
```

---

folder layout:
```
├── firmware/           # nRF52840 Zephyr C firmware
├── gateway/             # Rust gateway + Python bridge scripts for RPi
├── client/               # Rust client (display/db/crypto)
├── docs/                 # The "Senior Engineer" stuff
│   ├── architecture/   # Diagrams and Schematics
│   └── analysis/         # Power and Threat models
├── LICENSE
└── README.md
```