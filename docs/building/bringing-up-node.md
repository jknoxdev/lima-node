
## Bringing Up the L.I.M.A. Node
This project uses a Local Manifest topology. Follow these steps to initialize the workspace and install the necessary dependencies for the nRF52840.

1. Prerequisite: Python Environment
We recommend using a virtual environment to avoid dependency drift.


### Create and activate a clean environment

```bash
python3 -m venv .venv
source .venv/bin/activate
```

### Install West (Zephyr's meta-tool)

```bash
pip install west
```

2. Initialize the Workspace
The west.yml in this repository acts as the master blueprint for the entire SDK.
Bash

### Initialize the workspace using this repo as the local manifest

```bash
west init -l lima-node
```

### Pull the Nordic Connect SDK (NCS), Zephyr, and HAL modules

```bash
west update
```

3. Install SDK Requirements
Once the modules are downloaded, install the specific toolchain requirements:

```bash
pip install -r zephyr/scripts/requirements.txt
pip install -r nrf/scripts/requirements.txt
pip install -r bootloader/mcuboot/scripts/requirements.txt
```


4. Build & Verify

Test the toolchain by building the firmware for the nRF52840 MDK Dongle:


```bash
west build -b nrf52840_mdk lima-node/firmware
```



-----

folder layout:
```
├── src/
│   ├── firmware/       # nRF52840 C++/Arduino code
│   └── gateway/        # Python bridge for RPi
├── docs/               # The "Senior Engineer" stuff
│   ├── architecture/   # Diagrams and Schematics
│   └── analysis/       # Power and Threat models
├── tests/              # Validation scripts
├── LICENSE
└── README.md
```
