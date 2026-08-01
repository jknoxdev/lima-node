# ADR-003: Gateway Messaging — MQTT over CoAP / raw TCP / HTTP

**Status:** Accepted  
**Date:** 2026-02-25  
**Author:** jknoxdev

---

## Context

Once the BLE scanner on the gateway receives a signed event payload from a LIMA node, it needs an internal messaging layer to route that event from the scanner process to the event router, verifier, and egress handlers.

The messaging layer must:
- Decouple ingestion from processing (scanner shouldn't block on downstream processing)
- Support multiple subscribers (push notifier, SIEM forwarder, cloud uploader)
- Operate entirely locally — no cloud dependency for core event routing
- Be lightweight enough to run on Raspberry Pi Zero (512MB RAM, single core)
- Support topic-based filtering for future multi-node deployments

---

## Decision

**Selected: MQTT via Mosquitto broker (local, on-gateway)**

MQTT with a local Mosquitto broker is the internal event bus for the LIMA gateway.

---

## Alternatives Considered

### CoAP (Constrained Application Protocol)
- ✅ Designed for constrained devices — very lightweight
- ✅ REST-like semantics — easy to reason about
- ❌ Request/response model — not naturally pub/sub
- ❌ Observe pattern (pub/sub equivalent) is complex to implement reliably
- ❌ Less tooling and ecosystem vs MQTT
- ❌ Primary use case is device-to-cloud, not local IPC

### Raw TCP / Unix Sockets
- ✅ Zero overhead — maximum performance
- ✅ No broker dependency
- ❌ No pub/sub — requires custom fan-out logic for multiple consumers
- ❌ No topic filtering — all consumers receive all events
- ❌ Tight coupling between producer and consumers
- ❌ Reinventing the wheel — MQTT solves exactly this problem

### HTTP / REST
- ✅ Universal, well understood
- ✅ Easy to debug with curl / browser
- ❌ Request/response — polling required for event consumption
- ❌ High overhead per message vs MQTT
- ❌ Not designed for real-time event streaming
- ❌ Each consumer needs its own endpoint — no native fan-out

### ZeroMQ
- ✅ High performance, flexible messaging patterns
- ✅ No broker required
- ❌ More complex API than MQTT
- ❌ No persistence — messages lost if consumer offline
- ❌ Overkill for LIMA's event volume

---

## Rationale

MQTT is the natural fit for LIMA's gateway architecture:

| Requirement        | MQTT             | CoAP | Raw TCP  | HTTP |
| --------------------| ------------------| ------| ----------| ------|
| Pub/sub fan-out    | ✅ native         | ⚠️    | ❌ custom | ❌    |
| Topic filtering    | ✅ `lima/#`       | ❌    | ❌        | ❌    |
| RPi Zero footprint | ✅ Mosquitto ~3MB | ✅    | ✅        | ✅    |
| SIEM compatibility | ✅ native         | ⚠️    | ❌        | ✅    |
| QoS / persistence  | ✅                | ⚠️    | ❌        | ❌    |
| Local only capable | ✅                | ✅    | ✅        | ✅    |

**Topic structure** enables clean multi-node scaling without code changes:
```
lima/events/{node_id}     ← integrity events
lima/status/{node_id}     ← heartbeat / battery
lima/fault/{node_id}      ← fault notifications
```

**Mosquitto** specifically — runs in ~3MB RAM, starts in <1s, has been in production since 2008, and is the reference MQTT broker for IoT deployments. Zero configuration overhead for a single-gateway deployment.

**SIEM compatibility** is a bonus — most SIEM platforms (Splunk, ELK, Graylog) have native MQTT input plugins, making the gateway-to-SIEM integration a configuration task rather than a development task.

---

## Consequences

- **Good:** Native pub/sub fan-out to all consumers, topic-based node filtering, SIEM-compatible
- **Good:** Mosquitto is battle-tested, minimal resource footprint on RPi Zero
- **Good:** QoS levels available for guaranteed delivery to local subscribers
- **Bad:** Broker is a single point of failure on the gateway — mitigated by SQLite audit log
- **Bad:** Adds a process dependency — Mosquitto must be running before scanner starts
- **Neutral:** MQTT over TLS for egress (gateway to cloud) uses same protocol end-to-end
