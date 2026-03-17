## Hardware Requirements — BLE Adapter

The gateway adapter MUST support BLE 5.0 extended advertising (ADV_EXT_IND / 
Secondary PHY LE 2M). The node broadcasts on extended ADV only — legacy ADV 
is not supported due to 90-byte payload size (64-byte sig alone exceeds 31-byte 
legacy limit).

Confirmed NOT working:
- Raspberry Pi 5 onboard Cypress (kernel driver limitation)
- Generic BR/EDR USB dongles
- TP-Link adapters
- Raytac/Amazon cheap BLE dongles

Confirmed working:
- ASUS BT500 (pending verification)
- nRF52840 with proper HCI firmware (WIP)

Pi 5 kernel note: kernel 6.12.x breaks BLE scanning entirely. Downgrade to 
6.6.x via rpi-update and pin packages.