import asyncio
from bleak import BleakScanner
from bleak.backends.device import BLEDevice
from bleak.backends.scanner import AdvertisementData
from cryptography.hazmat.primitives.asymmetric.ec import (
    ECDSA, EllipticCurvePublicKey, SECP256R1
)
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature
from cryptography.hazmat.backends import default_backend
from cryptography.hazmat.primitives.asymmetric import ec
import struct

NODE_MAC = "E3:79:63:12:EF:B1"
NODE_PUBKEY_HEX = "048da87d0a4ddfc416c401826ed8ea0db29ec36513506969b88c8379de06e3103e42a39e66e8f3e7aa62d2aa24184d88e11f2c7aaa9de8a04884905b59ed487fd7"

def load_pubkey():
    pub_bytes = bytes.fromhex(NODE_PUBKEY_HEX)
    return ec.EllipticCurvePublicKey.from_encoded_point(SECP256R1(), pub_bytes)

def verify_payload(data: bytes) -> bool:
    if len(data) < 64:
        return False
    msg = data[:-64]
    sig_raw = data[-64:]
    # convert raw r||s to DER for cryptography lib
    r = int.from_bytes(sig_raw[:32], 'big')
    s = int.from_bytes(sig_raw[32:], 'big')
    try:
        from cryptography.hazmat.primitives.asymmetric.utils import encode_dss_signature
        sig_der = encode_dss_signature(r, s)
        pubkey.verify(sig_der, msg, ECDSA(hashes.SHA256()))
        return True
    except Exception:
        return False

pubkey = load_pubkey()

def callback(device: BLEDevice, adv: AdvertisementData):
    if device.address.upper() == NODE_MAC.upper():
        for mfr_id, data in adv.manufacturer_data.items():
            valid = verify_payload(data)
            status = "✅ VALID" if valid else "❌ INVALID"
            print(f"[LIMA] rssi: {adv.rssi}  len: {len(data)}  sig: {status}")
            print(f"       data: {data[:-64].hex()}")
            print(f"       sig:  {data[-64:].hex()}")

async def main():
    print(f"[*] Scanning for LIMA node {NODE_MAC}...")
    scanner = BleakScanner(callback, adapter="hci1, scanning_mode="active")
    async with scanner:
        await asyncio.sleep(60)

asyncio.run(main())