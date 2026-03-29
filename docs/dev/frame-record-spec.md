
Sensor/Event frames (0x01–0x1F)

```haskell
0x01  SENSOR_EVENT      — current, LER inside
0x02  SENSOR_BATCH      — multiple LERs in one frame (v2)
0x03  ANOMALY_ALERT     — ML-triggered, MGM240 (v2)
```
Lifecycle frames (0x20–0x3F)

```haskell
0x20  KEEPALIVE         — proof of life, no payload
0x21  BOOT              — node just powered on
0x22  SHUTDOWN          — clean power down
0x23  WATCHDOG_RESET    — unclean restart, flag for audit
```
Provisioning frames (0x40–0x5F)

```haskell
0x40  KEY_ROTATION      — ECDH key exchange (v2)
0x41  CONFIG_ACK        — node acknowledges config push
```
Mesh frames (0x60–0x7F)

```haskell
0x60  MESH_RELAY        — forwarded frame, original node_id preserved
0x61  MESH_TOPOLOGY     — node broadcasting neighbors
```
Diagnostic frames (0x80–0x9F)

```haskell
0x80  SELF_TEST_PASS
0x81  SELF_TEST_FAIL
0x82  SENSOR_FAULT      — IMU/baro offline
```
Reserved (0xA0–0xFE) — future use

```haskell
0xFF — broadcast/wildcard, useful for mesh
```