#!/usr/bin/env python3
import subprocess
import struct
import time
import sys

# NVS sector config — must match firmware
NVS_SECTOR_BASE = 0x000F6000  # adjust to your NVS partition address
NVS_KEY_ID      = 2           # LIMA_NVS_PROVISION_TIME_ID

def main():
    ts = int(time.time())
    print(f"[LIMA] provisioning time: {ts} ({time.ctime(ts)})")
    
    # Pack as little-endian uint32
    data = struct.pack("<I", ts)
    hex_data = " ".join(f"{b:02x}" for b in data)
    
    # Write via nrfjprog
    addr = NVS_SECTOR_BASE  # TODO: calculate exact NVS entry address
    cmd = ["nrfjprog", "--memwr", f"0x{addr:08X}",
           "--val", f"0x{int.from_bytes(data, 'little'):08X}",
           "--snr", sys.argv[1] if len(sys.argv) > 1 else ""]
    
    print(f"[LIMA] writing {hex_data} to 0x{addr:08X}")
    subprocess.run(cmd, check=True)
    print("[LIMA] provisioning complete — power cycle to apply")

if __name__ == "__main__":
    main()