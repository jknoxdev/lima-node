# ADR-002: Wireless Protocol — BLE 5.0 over Thread / Zigbee / LoRa / WiFi

**Status:** Accepted  
**Date:** 2026-02-25  
**Author:** jknoxdev

---

## Context

LIMA nodes must transmit signed integrity events to the gateway reliably, at low power, with sufficient range to cover target deployment environments including:
- Server room / network rack enclosures
- Vehicle interiors (Faraday-like environments)
- Industrial cabinets and control panels
- Perimeter installations up to ~100m from gateway

The wireless protocol choice determines range, power consumption, gateway complexity, and the overall attack surface of the air interface.

---

## Decision

**Selected: Bluetooth Low Energy 5.0 with Coded PHY (Long Range mode)**

BLE 5.0 Coded PHY is the primary air interface for LIMA v1.0 node-to-gateway communication.

---

## Alternatives Considered

### Thread (802.15.4 mesh)
- ✅ Mesh topology — nodes can relay for each other
- ✅ IPv6 native — direct internet addressability
- ✅ Also supported on nRF52840
- ❌ Requires Thread Border Router — adds gateway complexity
- ❌ Mesh routing adds non-deterministic latency to security events
- ❌ Larger attack surface — mesh peers can be spoofed or injected
- ❌ Overkill for single-gateway deployments

### Zigbee (802.15.4)
- ✅ Mature, widely deployed in industrial settings
- ✅ Low power, mesh capable
- ❌ Fragmented ecosystem — coordinator hardware required
- ❌ Known security vulnerabilities in legacy deployments
- ❌ Less native support in Zephyr/NCS vs BLE
- ❌ Dying protocol — industry moving toward Thread/Matter

### LoRa / LoRaWAN
- ✅ Exceptional range — kilometers
- ✅ Very low power
- ❌ Low bandwidth — insufficient for signed cryptographic payloads at reasonable intervals
- ❌ Requires LoRa gateway infrastructure — not universally available
- ❌ External radio module required on nRF52840 — adds BOM complexity
- ❌ High latency — unsuitable for real-time security alerting

### WiFi (802.11)
- ✅ High bandwidth, universal infrastructure
- ✅ Direct IP connectivity to MQTT broker
- ❌ Radio power consumption — 10-100x BLE — eliminates battery deployment
- ❌ Connects to existing WiFi infrastructure — violates air-gap principle
- ❌ SSID/credential management at scale is an operational burden
- ❌ Larger attack surface — WPA2/3 complexity

---

## Rationale

BLE 5.0 Coded PHY satisfies all requirements without gateway infrastructure complexity:

| Requirement             | BLE 5.0 Coded | Thread          | LoRa   | WiFi |
| -------------------------| ---------------| -----------------| --------| ------|
| Range > 50m indoors     | ✅ ~100m       | ✅ mesh          | ✅ km   | ⚠️    |
| Battery life > 1yr      | ✅             | ✅               | ✅      | ❌    |
| No extra infrastructure | ✅             | ❌ Border Router | ❌ GW   | ❌ AP |
| nRF52840 native         | ✅             | ✅               | ❌      | ❌    |
| Signed payload support  | ✅             | ✅               | ⚠️ size | ✅    |
| Zephyr first-class      | ✅             | ✅               | ⚠️      | ⚠️    |

**Coded PHY specifically** — BLE 5.0 Coded PHY uses forward error correction to double effective range vs BLE 4.2 at identical transmit power. This is critical for metal enclosure deployments where RF attenuation is significant.

The gateway (RPi Zero) acts as a simple BLE scanner — no pairing, no connection state, just advertisement scanning. This is the minimal viable attack surface for the air interface.

---

## Consequences

- **Good:** Native nRF52840 support, minimal gateway hardware, Coded PHY extended range
- **Good:** Advertisement-only mode — no connection state means no connection hijacking attack surface
- **Good:** Well-understood protocol with mature Zephyr BLE stack
- **Bad:** Single-hop only — gateway must be within BLE range of all nodes
- **Bad:** 2.4GHz band — subject to interference in dense WiFi environments
- **Neutral:** No acknowledgement from gateway — TX is fire-and-forget (mitigated by local SQLite on gateway)
