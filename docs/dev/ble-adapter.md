### Confirming BLE 5.0 Extended Advertising Support (Linux)

Required for receiving LIMA's ADV_EXT_IND packets (90-byte payload, Secondary PHY LE 2M).
```bash
# List controllers — look for your adapter (ASUS BT500 = Realtek, manufacturer 93)
sudo btmgmt info

# Query LE supported features — returns 8-byte bitmask
sudo hcitool -i hci1 cmd 0x08 0x0003

# Response bytes 2-9 are the feature mask. Bit 12 = LE Extended Advertising.
# Confirmed BT500 response: FD 77 FE F7 8F 00 00 00 — bit 12 SET ✅
```

**ASUS BT500 (RTL8761B) on hci1 — confirmed compatible.**  
Pi 5 onboard Cypress (hci0) — confirmed NOT compatible.  
Kernel must be pinned to 6.6.77 (6.12.x has silent BLE scanning bug).