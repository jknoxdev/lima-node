import asyncio
import struct
from bleak import BleakScanner
from bleak.backends.device import BLEDevice
from bleak.backends.scanner import AdvertisementData
from cryptography.hazmat.primitives.asymmetric.ec import ECDSA, SECP256R1
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric.utils import encode_dss_signature
from cryptography.hazmat.primitives.asymmetric import ec

NODE_MAC = "E3:79:63:12:EF:B1"
NODE_PUBKEY_HEX = "048da87d0a4ddfc416c401826ed8ea0db29ec36513506969b88c8379de06e3103e42a39e66e8f3e7aa62d2aa24184d88e11f2c7aaa9de8a04884905b59ed487fd7"

def load_pubkey():
    pub_bytes = bytes.fromhex(NODE_PUBKEY_HEX)
    return ec.EllipticCurvePublicKey.from_encoded_point(SECP256R1(), pub_bytes)

pubkey = load_pubkey()

def zephyr_hexdump(data: bytes, indent: str = "                                   ") -> str:
    lines = []
    for i in range(0, len(data), 16):
        chunk = data[i:i+16]
        hex_left  = " ".join(f"{b:02x}" for b in chunk[:8]).ljust(23)
        hex_right = " ".join(f"{b:02x}" for b in chunk[8:]).ljust(23)
        ascii_str = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        ascii_left  = ascii_str[:8]
        ascii_right = ascii_str[8:]
        lines.append(f"{indent}{hex_left}  {hex_right} |{ascii_left} {ascii_right}|")
    return "\n".join(lines)

def parse_adv_payload(data: bytes) -> dict:
    # adv_payload layout (inferred — update once ble.c confirmed):
    # [0]    event_type  uint8
    # [1]    version/pad uint8
    # [2-5]  sequence    uint32 LE
    # [6-9]  timestamp   uint32 LE (ms)
    # [10-13] accel_g    float32 LE
    # [14-17] delta_pa   float32 LE
    # [18-23] node_id    uint8[6]
    evt       = data[0]
    seq       = struct.unpack_from("<I", data, 2)[0]
    ts_ms     = struct.unpack_from("<I", data, 6)[0]
    accel_g   = struct.unpack_from("<f", data, 10)[0]
    delta_pa  = struct.unpack_from("<f", data, 14)[0]
    node_id   = data[18:24]
    node_str  = ":".join(f"{b:02X}" for b in reversed(node_id))
    return dict(evt=evt, seq=seq, ts_ms=ts_ms, accel_g=accel_g,
                delta_pa=delta_pa, node_str=node_str)

def reconstruct_lima_payload(data: bytes) -> bytes:
    evt_type = data[0]
    seq      = data[2:6]    # uint32 LE
    ts_ms    = data[6:10]   # uint32 LE
    accel    = data[10:14]  # float32 LE
    delta_pa = data[14:18]  # float32 LE
    node_id  = data[18:24]  # uint8[6]
    # reassemble in lima_payload_t order
    return node_id + bytes([evt_type, 0x00]) + seq + ts_ms + accel + delta_pa

def verify_payload(data: bytes, sig_raw: bytes) -> bool:
    msg = reconstruct_lima_payload(data)
    r = int.from_bytes(sig_raw[:32], 'big')
    s = int.from_bytes(sig_raw[32:], 'big')
    try:
        sig_der = encode_dss_signature(r, s)
        pubkey.verify(sig_der, msg, ECDSA(hashes.SHA256()))
        return True
    except Exception:
        return False

def callback(device: BLEDevice, adv: AdvertisementData):
    if device.address.upper() != NODE_MAC.upper():
        return
    for mfr_id, raw in adv.manufacturer_data.items():
        if len(raw) < 64:
            continue
        data   = raw[:-64]
        sig    = raw[-64:]
        valid  = verify_payload(data, sig)
        status = "✅ VALID" if valid else "❌ INVALID"
        p      = parse_adv_payload(data)

        # reconstruct full adv_payload with mfr_id prefix (matches firmware log)
        full_adv = bytes([mfr_id & 0xff, (mfr_id >> 8) & 0xff]) + raw

        print(f"[LIMA] node={p['node_str']} evt=0x{p['evt']:02X} "
              f"seq={p['seq']} accel={p['accel_g']:.2f} "
              f"delta_pa={p['delta_pa']:.2f} "
              f"sig[0..3]={sig[:4].hex().upper()}  {status}")
        print(f"  adv_payload:")
        print(zephyr_hexdump(full_adv))
        print()


async def main():
    print(f"[*] Scanning for LIMA node {NODE_MAC}...")
    scanner = BleakScanner(callback, adapter="hci1", scanning_mode="active")
    async with scanner:
        await asyncio.sleep(9000)

asyncio.run(main())