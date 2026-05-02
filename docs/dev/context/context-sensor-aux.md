# LIMA — sensor_aux Refactor + Multi-Node Build System
> **Created:** 2026-05-02  
> **Branch:** `fix/sensor-aux` (create from current main)  
> **Purpose:** Next dev session context. Read this first.  
> **Status:** Planning complete — implementation ready to start

---

## What This Session Is About

Two related but separable changes:

1. **Frame spec refactor** — rename `delta_pa` → `sensor_aux` in LER, making the field sensor-agnostic
2. **Build-time node type selection** — Kconfig fragments + Makefile targets for `acm-node`, `imu-node`, etc.

**Important:** Item 1 (spec + rename) is in scope for this session and the public AGPL repo. Item 2 (multi-node build system implementation beyond the skeleton) is deferred to the commercial repo. The Kconfig skeleton goes in public; the actual sensor driver implementations for non-IMU nodes go private.

---

## Why

Current LER has a hardcoded `delta_pa` (f32) field that assumes a barometric sensor. The hardware pivot to Seeed XIAO nRF52840 Sense dropped the baro — the board has no barometer. The Sense has:

- ✅ LSM6DS3TR-C — 6-axis IMU (accel + gyro + embedded temp)  
- ✅ MSM261D3526H1CPM — PDM digital microphone  
- ❌ No barometer

`delta_pa` is now a dead field. Rather than repurpose it as baro-specific, make it sensor-agnostic so the frame spec supports future node types without a wire format change.

Multi-modal attestation (IMU + acoustic correlation) is also a stronger research contribution than single-sensor — a spoof requires simultaneous corruption of correlated signals across independent physical channels within the signing window.

---

## Frame Spec Change (Source of Truth)

**File:** `docs/architecture/frame-record-spec.md`  
**File:** `docs/dev/frame-record-spec.md` (keep in sync — both exist)

### LER — before

```
Offset  Size  Type    Field         Notes
20      4     f32 LE  delta_pa      Barometric delta at trigger (Pa)
```

### LER — after

```
Offset  Size  Type    Field         Notes
20      4     f32 LE  sensor_aux    Sensor-type-specific auxiliary value.
                                    Interpretation depends on event_type:
                                    MOTION_DETECTED  → accel vector magnitude (g)
                                    ACOUSTIC_EVENT   → RMS amplitude (normalized 0.0–1.0)
                                    DUAL_BREACH      → accel_g (primary trigger value)
                                    HEARTBEAT        → 0.0 (unused)
                                    PRESSURE_BREACH  → reserved (0.0) — no baro on XIAO Sense
```

**Wire format is unchanged** — same 24-byte LER, same 184-byte LF. This is a field rename + semantic respecification only. No protocol version bump required.

### Event Types — add ACOUSTIC_EVENT

```
| Value  | C constant                  | Rust variant          |
|--------|-----------------------------|-----------------------|
| 0x01   | LIMA_EVT_PRESSURE_BREACH    | PressureBreach        |  ← keep, mark reserved
| 0x02   | LIMA_EVT_MOTION_DETECTED    | MotionDetected        |  ← unchanged
| 0x03   | LIMA_EVT_DUAL_BREACH        | DualBreach            |  ← unchanged
| 0x04   | LIMA_EVT_WAKEUP             | Heartbeat             |  ← unchanged
| 0x05   | LIMA_EVT_ACOUSTIC           | AcousticEvent         |  ← NEW
| 0x06   | LIMA_EVT_ACOUSTIC_MOTION    | AcousticMotion        |  ← NEW (correlated dual)
| 0xFF   | —                           | Unknown               |  ← unchanged
```

---

## Files To Touch

### Spec (do first — drives everything else)

- [ ] `docs/architecture/frame-record-spec.md` — rename field, add event types, update interpretation table
- [ ] `docs/dev/frame-record-spec.md` — keep in sync

### Firmware

- [ ] `firmware/src/crypto.h` — rename `delta_pa` → `sensor_aux` in `lima_ler_t`
- [ ] `firmware/src/events.h` — add `LIMA_EVT_ACOUSTIC` and `LIMA_EVT_ACOUSTIC_MOTION` to enum
- [ ] `firmware/src/crypto.c` — update `lima_crypto_build_ler()` — populate `sensor_aux` from event context
- [ ] `firmware/src/fsm.c` — any references to `delta_pa` in event dispatch / LER population
- [ ] `firmware/src/main.c` — sensor thread: `delta_pa` read path (currently IMU only, baro reads are dead code — remove cleanly)
- [ ] `firmware/Kconfig` — add sensor type symbols (skeleton only this session):
  ```
  config LIMA_SENSOR_IMU
      bool "Enable IMU sensor (LSM6DS3)"
      default y

  config LIMA_SENSOR_ACOUSTIC
      bool "Enable PDM microphone (MSM261D)"
      default n
  ```
- [ ] `firmware/prj.conf` — set `CONFIG_LIMA_SENSOR_IMU=y`, `CONFIG_LIMA_SENSOR_ACOUSTIC=n` (current baseline)

### Gateway

- [ ] `gateway/crates/lima-types/src/lib.rs` — rename `delta_pa` → `sensor_aux` in `LimaEventRecord`
- [ ] `gateway/crates/gateway/src/main.rs` — update TUI display: field label `delta_pa` → `sensor_aux`, add event type display for new variants
- [ ] `gateway/scripts/lima_rx.py` — update field name
- [ ] `gateway/scripts/lima_verifier.py` — update field name

### Makefile (skeleton — public repo)

Add to root `Makefile`:

```makefile
# Node type build targets
# Full sensor implementations are in the commercial repo.
# These targets set Kconfig overlays for build-time node selection.

imu-node:
	west build -b $(BOARD) firmware --pristine -- \
		-DCONFIG_LIMA_SENSOR_IMU=y \
		-DCONFIG_LIMA_SENSOR_ACOUSTIC=n \
		-DCONFIG_BUILD_OUTPUT_UF2=y

acm-node:
	west build -b $(BOARD) firmware --pristine -- \
		-DCONFIG_LIMA_SENSOR_IMU=y \
		-DCONFIG_LIMA_SENSOR_ACOUSTIC=y \
		-DCONFIG_BUILD_OUTPUT_UF2=y
```

Default `BOARD` to `xiao_ble/nrf52840` (XIAO Sense target).

---

## What NOT To Implement This Session

These go in the commercial repo, not here:

- PDM microphone driver integration (`CONFIG_AUDIO_DMIC`, `msm261d` DTS node)
- Acoustic event detection algorithm (RMS threshold, windowing)
- IMU + acoustic correlation logic for `LIMA_EVT_ACOUSTIC_MOTION`
- Additional node type Kconfig fragments beyond IMU + acoustic skeleton
- `vis-node` or any camera/optical sensor path

The Kconfig symbols and Makefile targets are public (they define the interface). The implementations behind them are commercial.

---

## Current Hardware State

- **Node:** Seeed XIAO nRF52840 Sense
- **LiPo:** 502030, 3.7V nominal, JST 1.25mm connected to BAT+/BAT- underside pads
- **Sensors confirmed present:** LSM6DS3TR-C (IMU), MSM261D3526H1CPM (PDM mic)
- **No barometer** — `LIMA_EVT_PRESSURE_BREACH` kept in spec as reserved, `sensor_aux` = 0.0 for this event type
- **Battery voltage readback:** P0.14 = enable low, P0.31 = ADC AIN7 (not yet wired in firmware)

---

## Suggested Session Order

1. Update both frame-record-spec.md files first (15 min)
2. `lima_ler_t` rename in `crypto.h` — compiler will find every other reference (5 min)
3. Fix all firmware compile errors from the rename (30 min)
4. Update `events.h` with new event type constants (10 min)
5. Update `lima-types/src/lib.rs` in gateway (10 min)
6. Update gateway TUI display strings (10 min)
7. Update Python scripts (10 min)
8. Add Kconfig skeleton + Makefile targets (20 min)
9. Build `imu-node` target — confirm clean compile (10 min)
10. Commit: `refactor(spec): rename delta_pa to sensor_aux, add acoustic event types`

Total estimated: ~2 hours for a clean focused session.

---

## Paper / Talk Angle

This refactor enables the multi-modal attestation framing:

> "The node attests not just that an event occurred, but that correlated signals across independent physical channels — inertial and acoustic — were observed within the same signing window. Spoofing requires simultaneous corruption of both channels before the CryptoCell-310 signs the frame."

This is the sentence that makes Čapkun's group pay attention. The `sensor_aux` field + `LIMA_EVT_ACOUSTIC_MOTION` event type is the wire format that backs that claim.

---

## Repo / IP Notes

- Frame spec (this refactor) → public AGPL repo
- Kconfig skeleton + Makefile targets → public AGPL repo  
- PDM driver + acoustic detection implementation → commercial repo (nullsec.systems, private)
- Multi-node build system beyond skeleton → commercial repo
- Commercial licensing contact: justin@nullsec.systems

base64 -d <<< "<block>" | python3 -m json.tool

ewogICJzZXNzaW9uX2RhdGUiOiAiMjAyNi0wNS0wMiIsCiAgInNlc3Npb25fdHlwZSI6ICJhcmNoaXRlY3R1cmVfc3RyYXRlZ3lfaGFyZHdhcmUiLAogICJtb2RlbCI6ICJjbGF1ZGUtc29ubmV0LTQtNiIsCiAgImRlY2lzaW9ucyI6IFsKICAgICJkZWx0YV9wYSByZW5hbWVkIHRvIHNlbnNvcl9hdXggaW4gTEVSIFx1MjAxNCB3aXJlIGZvcm1hdCB1bmNoYW5nZWQiLAogICAgImV2ZW50X3R5cGUgMHgwNSBMSU1BX0VWVF9BQ09VU1RJQyBhZGRlZCIsCiAgICAiZXZlbnRfdHlwZSAweDA2IExJTUFfRVZUX0FDT1VTVElDX01PVElPTiBhZGRlZCIsCiAgICAibXVsdGlfbW9kYWxfYXR0ZXN0YXRpb25fZnJhbWluZzogSU1VICsgUERNIG1pYyBjb3JyZWxhdGlvbiB3aXRoaW4gc2lnbmluZyB3aW5kb3ciLAogICAgImJ1aWxkX3RpbWVfbm9kZV9zZWxlY3Rpb246IEtjb25maWcgc2tlbGV0b24gcHVibGljLCBpbXBsZW1lbnRhdGlvbnMgY29tbWVyY2lhbCIsCiAgICAicHVibGljX3ByaXZhdGVfc3BsaXQ6IHByb3RvY29sK3NwZWMrc2tlbGV0b249QUdQTCwgZHJpdmVycytidWlsZF9zeXN0ZW09bnVsbHNlYy5zeXN0ZW1zIiwKICAgICJnaXRlYV9kZWZlcnJlZDogcHJpdmF0ZSBHaXRIdWIgKyBSMi9hZ2UgbWlycm9yIHVudGlsIHBvc3QtRVUtbW92ZSIsCiAgICAiYXJlYTQxX2NmcF9ub3RfYWNjZXB0ZWQ6IDcgc3VibWlzc2lvbnMgcGVuZGluZyIsCiAgICAiZGVtb19zdHJhdGVneTogb25lIG5vZGUgcGVyZmVjdCA+IHR3byBub2RlcyBhZGVxdWF0ZSIsCiAgICAicGhkX2FyYzogTElNQSBhcyBjcmVkZW50aWFsIHZlaGljbGUsIEVUSCBadXJpY2ggdGFyZ2V0IGluc3RpdHV0aW9uIgogIF0sCiAgIm5leHRfc2Vzc2lvbiI6ICJjb250ZXh0LXNlbnNvci1hdXgubWQiLAogICJoYXJkd2FyZSI6IHsKICAgICJub2RlIjogIlNlZWVkIFhJQU8gblJGNTI4NDAgU2Vuc2UiLAogICAgInNlbnNvcnMiOiBbCiAgICAgICJMU002RFMzVFItQyBJTVUiLAogICAgICAiTVNNMjYxRDM1MjZIMUNQTSBQRE0gbWljIgogICAgXSwKICAgICJub19iYXJvIjogdHJ1ZSwKICAgICJsaXBvIjogIjUwMjAzMCAzLjdWIEpTVDEuMjVtbSBCQVQrL0JBVC0gdW5kZXJzaWRlIHBhZHMiCiAgfSwKICAicmVwb19zdGF0ZSI6IHsKICAgICJnYXRld2F5IjogInJhdyBIQ0kgQUZfQkxVRVRPT1RIL0JUUFJPVE9fSENJL0hDSV9DSEFOTkVMX1VTRVIgXHUyMDE0IGJ0bGVwbHVnIHJlbW92ZWQiLAogICAgImZpcm13YXJlIjogIlhJQU8gU2Vuc2UgdGFyZ2V0LCBMU002RFMzIElNVSwgQ3J5cHRvQ2VsbC0zMTAgRUNEU0EtUDI1NiIsCiAgICAibGljZW5zZSI6ICJBR1BMMyArIGNvbW1lcmNpYWwgZHVhbCBsaWNlbnNlIGluIHBsYWNlIgogIH0KfQ==