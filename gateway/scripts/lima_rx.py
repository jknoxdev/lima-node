"""
lima_rx.py — LIMA BLE scanner + LF outer signature verifier
Gateway script — passive scanner only, no decryption keys.

Wire format: docs/dev/frame-record-spec.md
LF = 184 bytes manufacturer-specific AD data:
  [0]      proto_version
  [1]      event_type
  [2-3]    reserved
  [4-15]   nonce (12B AES-256-GCM IV)
  [16-103] ciphertext (88B)
  [104-119] gcm_tag (16B)
  [120-183] outer_sig (64B ECDSA-P256)

Gateway role: verify outer_sig only. Store raw LF. Never decrypt.
"""

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
NODE_PUBKEY_HEX = (
    "048da87d0a4ddfc416c401826ed8ea0db29ec36513506969b88c8379de06e3103e"
    "42a39e66e8f3e7aa62d2aa24184d88e11f2c7aaa9de8a04884905b59ed487fd7"
)

# ── LF layout constants ───────────────────────────────────────────────────────

LF_LEN           = 184
LF_SIGNED_BYTES  = 120   # outer_sig covers LF[0..120]
OUTER_SIG_LEN    = 64

LF_OFFSET_PROTO      = 0
LF_OFFSET_EVENT_TYPE = 1
LF_OFFSET_NONCE      = 4
LF_OFFSET_CIPHERTEXT = 16
LF_OFFSET_GCM_TAG    = 104
LF_OFFSET_OUTER_SIG  = 120

EVENT_TYPES = {
    0x01: "PRESSURE_BREACH",
    0x02: "MOTION_DETECTED",
    0x03: "DUAL_BREACH",
    0x04: "HEARTBEAT",
}

# ── Crypto ────────────────────────────────────────────────────────────────────

def load_pubkey():
    pub_bytes = bytes.fromhex(NODE_PUBKEY_HEX)
    return ec.EllipticCurvePublicKey.from_encoded_point(SECP256R1(), pub_bytes)

pubkey = load_pubkey()

def verify_outer_sig(lf: bytes) -> bool:
    """Verify ECDSA-P256 outer signature over LF[0..120]."""
    if len(lf) < LF_LEN:
        return False
    signed = lf[:LF_SIGNED_BYTES]
    sig_raw = lf[LF_OFFSET_OUTER_SIG:LF_OFFSET_OUTER_SIG + OUTER_SIG_LEN]
    r = int.from_bytes(sig_raw[:32], 'big')
    s = int.from_bytes(sig_raw[32:], 'big')
    try:
        sig_der = encode_dss_signature(r, s)
        pubkey.verify(sig_der, signed, ECDSA(hashes.SHA256()))
        return True
    except Exception:
        return False

# ── Helpers ───────────────────────────────────────────────────────────────────

def zephyr_hexdump(data: bytes, indent: str = "  ") -> str:
    lines = []
    for i in range(0, len(data), 16):
        chunk = data[i:i+16]
        hex_left  = " ".join(f"{b:02x}" for b in chunk[:8]).ljust(23)
        hex_right = " ".join(f"{b:02x}" for b in chunk[8:]).ljust(23)
        ascii_str = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        lines.append(f"{indent}{i:04x}  {hex_left}  {hex_right} |{ascii_str[:8]} {ascii_str[8:]}|")
    return "\n".join(lines)

def parse_lf_header(lf: bytes) -> dict:
    """Parse unencrypted LF header fields (pre-filter only — no decryption here)."""
    proto   = lf[LF_OFFSET_PROTO]
    evt     = lf[LF_OFFSET_EVENT_TYPE]
    nonce   = lf[LF_OFFSET_NONCE:LF_OFFSET_NONCE + 12].hex()
    evt_str = EVENT_TYPES.get(evt, f"UNKNOWN(0x{evt:02X})")
    return dict(proto=proto, evt=evt, evt_str=evt_str, nonce=nonce)

# ── Scanner callback ──────────────────────────────────────────────────────────

def callback(device: BLEDevice, adv: AdvertisementData):
    if device.address.upper() != NODE_MAC.upper():
        return

    for mfr_id, raw in adv.manufacturer_data.items():
        # strip company_id (2B) prepended by bleak — LF starts after
        lf = raw  # bleak strips company_id, raw = LF bytes
        if len(lf) != LF_LEN:
            print(f"[LIMA] unexpected len={len(lf)} (expected {LF_LEN}) — skipping")
            continue

        valid  = verify_outer_sig(lf)
        status = "✅ OUTER SIG VALID" if valid else "❌ OUTER SIG INVALID"
        h      = parse_lf_header(lf)

        print(f"[LIMA] proto=0x{h['proto']:02X} evt={h['evt_str']} "
              f"nonce={h['nonce'][:12]}...  rssi={adv.rssi}  {status}")

        if not valid:
            print(f"  [!] TAMPER ALERT — outer signature failed")

        print(f"  raw LF ({len(lf)}B):")
        print(zephyr_hexdump(lf))
        print()

# ── Main ──────────────────────────────────────────────────────────────────────

async def main():
    print(f"[*] LIMA gateway scanner — listening for node {NODE_MAC}")
    print(f"[*] Role: outer sig verify only — no decryption keys held here")
    print()
    scanner = BleakScanner(callback, adapter="hci1", scanning_mode="active")
    async with scanner:
        await asyncio.sleep(9000)

asyncio.run(main())
