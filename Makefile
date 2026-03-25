VENV := ./.venv
PYTHON := $(VENV)/bin/python
WEST := $(VENV)/bin/west
BOARD := nrf52840dk/nrf52840
BUILD_DIR := lima-node/firmware/build

export PATH := $(CURDIR)/$(VENV)/bin:$(PATH)

.PHONY: build flash menuconfig clean init

init:
	python3 -m venv $(VENV)
	$(PYTHON) -m pip install --upgrade pip
	$(PYTHON) -m pip install west
	$(PYTHON) -m pip install -r zephyr/scripts/requirements.txt

build:
	$(WEST) build -b $(BOARD) lima-node/firmware --build-dir $(BUILD_DIR) --pristine

flash:
	$(WEST) flash --runner jlink --build-dir $(BUILD_DIR)

menuconfig:
	$(WEST) build -t menuconfig -b $(BOARD) firmware --build-dir $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)

monitor:
	tio -l --log-file lima-node/docs/logs/debug-$(shell date +%Y%m%d-%H%M%S).log /dev/ttyACM0

flash-monitor:
	$(WEST) flash --runner jlink --build-dir $(BUILD_DIR)
	tio -l --log-file lima-node/docs/logs/debug-$(shell date +%Y%m%d-%H%M%S).log /dev/ttyACM0

archive-logs:
	@if ls lima-node/docs/logs/*.log 2>/dev/null; then \
		tar -rvf lima-node/docs/logs/debug.tar.gz lima-node/docs/logs/*.log --remove-files; \
	else \
		echo "No logs to archive"; \
	fi

provision:
	$(WEST) build -b $(BOARD) lima-node/firmware --build-dir $(BUILD_DIR) --pristine \
		-- -DCONFIG_LIMA_PROVISION_UNIX_TIME=$(shell date +%s) \
		-DCONFIG_LIMA_FORCE_PROVISION=y
	$(WEST) flash --runner jlink --build-dir $(BUILD_DIR)
	tio -l --log-file lima-node/docs/logs/debug-$(shell date +%Y%m%d-%H%M%S).log /dev/ttyACM0