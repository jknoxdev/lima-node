import asyncio
from bleak import BleakScanner
from bleak.backends.device import BLEDevice
from bleak.backends.scanner import AdvertisementData

NODE_MAC = "E3:79:63:12:EF:B1"  # your node's MAC

def callback(device: BLEDevice, adv: AdvertisementData):
    if device.address.upper() == NODE_MAC.upper():
        print(f"[LIMA] device: {device.address}  rssi: {adv.rssi}")
        print(f"       raw mfr data: {adv.manufacturer_data}")
        print(f"       service data: {adv.service_data}")
        print(f"       raw bytes:    {adv.manufacturer_data.values()}")

async def main():
    print(f"[*] Scanning for LIMA node {NODE_MAC}...")
    scanner = BleakScanner(
        callback,
        adapter="hci1",
        scanning_mode="passive"
    )
    async with scanner:
        await asyncio.sleep(30)

asyncio.run(main())