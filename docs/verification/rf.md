# RF Verification — BLE Extended Advertising

This document defines the procedure used to verify that the L.I.M.A node
is correctly transmitting signed BLE Extended Advertising packets.

The goal is to independently confirm:

1. The node is transmitting extended advertising PDUs
2. The AUX_ADV_IND packet contains the full payload
3. The payload includes the expected signed node data
4. The payload can be decoded and verified by an independent receiver

This verification is required because BLE extended advertising uses
secondary channels which are difficult to observe using standard
consumer BLE sniffers.

---

# Architecture

BLE extended advertising separates control and payload data across
primary and secondary advertising channels.

```text
Primary channels (37,38,39)

ADV_EXT_IND
   ↓
AuxPtr → Secondary channel

Secondary channels

AUX_ADV_IND
   ↓
Full payload
```

For LIMA nodes the AUX_ADV_IND packet carries:

```text
[header]
[node_id]
[timestamp]
[sensor data]
[signature]
```

Current payload size:

```text
~188 bytes
```

---

# Problem Statement

Most low-cost BLE sniffers (including Nordic nRF Sniffer) monitor
one channel at a time and follow connections heuristically.

With extended advertising this creates a verification gap:

```text
ADV_EXT_IND captured
    ↓
AuxPtr indicates secondary channel
    ↓
Sniffer misses AUX_ADV_IND
```

Result:

The sniffer may display the advertising event but not the payload.

This makes it unreliable as proof that the full signed packet
was actually transmitted.

---

# Verification Strategy

RF verification uses two independent validation paths.

```text
TX verification
RX verification
```

Both must succeed.

---

# TX Verification (radio proof)

TX verification confirms the radio actually transmitted the payload.

Preferred tools:

```text
Silicon Labs PTI
Network Analyzer
```

Architecture:

```text
LIMA node (nRF52840)
        ↓
BLE radio TX
        ↓
RF packet
        ↓
PTI capture
        ↓
Network Analyzer
```

PTI provides:

* TX timestamps
* payload bytes
* RSSI
* CRC status

Unlike over-the-air sniffing, PTI captures packets directly
from the radio interface.

Verification criteria:

```text
AUX_ADV_IND detected
payload length ≈ expected size
payload contents match expected structure
signature bytes present
```

---

# RX Verification (functional proof)

RX verification confirms another device can reconstruct
the payload.

Receiver device:

```text
nRF52840 dev board
```

Receiver firmware:

```text
BLE scanner
extended advertising enabled
payload dump enabled
```

Flow:

```text
LIMA node TX
      ↓
Receiver scans
      ↓
Extended advertising received
      ↓
Payload reconstructed
      ↓
Signature verification
```

Verification criteria:

```text
payload length correct
node id correct
signature verification passes
```

---

# Receiver Debug Output

Example log:

```text
EXT ADV RECEIVED
len: 188

node_id: 0xA1B2
timestamp: 171234123

signature: OK
```

---

# Failure Modes

Common BLE extended advertising failures:

### Secondary channel not followed

```text
ADV_EXT_IND seen
AUX_ADV_IND missing
```

Cause:

sniffer channel hopping limitations.

---

### Payload fragmentation

Large payloads may be fragmented.

Symptoms:

```text
partial payload
missing bytes
signature failure
```

---

### Timing mismatch

Receiver misses AUX packet window.

Symptoms:

```text
intermittent reception
```

---

# Validation Checklist

Before marking RF verification complete:

```text
[ ] ADV_EXT_IND visible
[ ] AUX_ADV_IND visible
[ ] payload size correct
[ ] node_id present
[ ] signature present
[ ] receiver verifies signature
```

---

# Recommended Hardware

Minimum setup:

```text
1x nRF52840 (LIMA node)
1x nRF52840 (receiver)
```

Optional advanced verification:

```text
1x Silicon Labs dev kit (PTI capture)
```

---

# Future Improvements

Future RF validation tooling may include:

* automated packet signature verification
* Wireshark BLE dissector plugin
* long-term packet reliability testing
* RF interference analysis