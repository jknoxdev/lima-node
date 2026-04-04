import asyncio
from bleak import BleakScanner
from bleak.backends.device import BLEDevice
from bleak.backends.scanner import AdvertisementData

NODE_MAC = "E3:79:63:12:EF:B1"  # your node's MAC

def callback(device: BLEDevice, adv: AdvertisementData):
    if device.address.upper() == NODE_MAC.upper():
        for mfr_id, data in adv.manufacturer_data.items():
            print(f"[LIMA] rssi: {adv.rssi}  mfr_id: 0x{mfr_id:04x}  len: {len(data)}")
            print(f"       hex: {data.hex()}")

async def main():
    print(f"[*] Scanning for LIMA node {NODE_MAC}...")
    scanner = BleakScanner(
        callback,
        adapter="hci0",
        scanning_mode="active"
    )
    async with scanner:
        await asyncio.sleep(3000)

asyncio.run(main())