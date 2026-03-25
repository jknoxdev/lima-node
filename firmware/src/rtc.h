/*
 * L.I.M.A. — Local Integrity Multi-modal Architecture
 * rtc.h — DS3231 RTC management, NVS epoch backup, tamper-epoch anchor
 *
 * Provides:
 *   - Hardware RTC read/write (DS3231 over I2C via Zephyr counter API)
 *   - NVS-backed epoch as fallback and skew-comparison tamper signal
 *   - Wall-clock timestamp for lima_payload_t (replaces k_uptime_get_32())
 *   - Deep-sleep wakeup alarm arming (extracted from fsm_hw_enter_deep_sleep)
 *
 * Boot priority:
 *   1. DS3231 hardware (battery-backed, authoritative)
 *   2. NVS last-known epoch (survives MCU power loss, not RTC battery loss)
 *   3. CONFIG_LIMA_PROVISION_UNIX_TIME (Kconfig bake-in, last resort)
 *
 * Tamper signal:
 *   If |rtc_epoch − nvs_epoch| > LIMA_RTC_SKEW_THRESHOLD_S on boot,
 *   lima_rtc_init() returns LIMA_RTC_ERR_SKEW and the caller should
 *   post LIMA_EVT_TAMPER_DETECTED before continuing.
 *
 * Copyright (c) 2026 Justin Knox <justin@nullsec.systems>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef LIMA_RTC_H
#define LIMA_RTC_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Return codes ─────────────────────────────────────────────────────────── */

/** Normal success */
#define LIMA_RTC_OK             0

/** DS3231 device not ready or I2C error */
#define LIMA_RTC_ERR_HW        -1

/** Both RTC and NVS unavailable; fell back to Kconfig bake-in */
#define LIMA_RTC_ERR_FALLBACK  -2

/**
 * Epoch skew between DS3231 and NVS exceeded LIMA_RTC_SKEW_THRESHOLD_S.
 * RTC value was used; caller should post LIMA_EVT_TAMPER_DETECTED.
 * Time is still valid — do not treat as fatal.
 */
#define LIMA_RTC_ERR_SKEW      -3

/* ── Tuning constants ─────────────────────────────────────────────────────── */

/**
 * Maximum tolerable difference (seconds) between DS3231 and NVS epoch
 * before declaring a skew tamper event.  5 minutes is generous for normal
 * power-cycle drift; tighten for high-security deployments.
 */
#define LIMA_RTC_SKEW_THRESHOLD_S   300U

/**
 * NVS epoch flush interval (ms).  Writing every 60 s limits wear while
 * keeping last-known time within 1 minute of truth after power loss.
 */
#define LIMA_RTC_NVS_FLUSH_MS       60000U

/** Format current wall-clock time into buf as "YYYY-MM-DD HH:MM:SS UTC" */
void lima_rtc_format_now(char *buf, size_t len);

/* ── Public API ───────────────────────────────────────────────────────────── */



/**
 * @brief  Initialise the RTC subsystem.
 *
 * Must be called after NVS/settings_load() and before fsm_init().
 * Reads DS3231, compares against NVS backup, resolves fallback chain,
 * anchors the boot epoch, and schedules periodic NVS flushes.
 *
 * @return LIMA_RTC_OK       — DS3231 time accepted, no anomaly.
 *         LIMA_RTC_ERR_SKEW — DS3231 and NVS diverge; caller posts tamper.
 *         LIMA_RTC_ERR_FALLBACK — both sources dead; Kconfig bake-in used.
 *         LIMA_RTC_ERR_HW  — DS3231 not ready (bus error).
 */
int lima_rtc_init(void);

/**
 * @brief  Return current Unix epoch (seconds).
 *
 * Computed as boot_epoch + elapsed_uptime_seconds.
 * Valid only after lima_rtc_init().
 */
uint32_t lima_rtc_get_epoch(void);

/**
 * @brief  Return wall-clock timestamp in milliseconds for lima_payload_t.
 *
 * Replaces k_uptime_get_32() in sensor events.  Returns real wall time
 * rather than uptime-since-boot, making audit log timestamps meaningful.
 */
uint64_t lima_rtc_timestamp_ms(void);

/**
 * @brief  Set the RTC epoch (called by gateway time-sync path).
 *
 * Writes to DS3231, updates NVS immediately, re-anchors boot_epoch.
 *
 * @param epoch  Unix time in seconds.
 * @return 0 on success, negative errno on failure.
 */
int lima_rtc_set_epoch(uint32_t epoch);

/**
 * @brief  Flush current epoch to Settings immediately.
 *
 * Called on clean shutdown or before deep sleep.
 * The periodic timer already handles normal operation.
 */
void lima_rtc_flush(void);

/**
 * @brief  Arm DS3231 alarm for deep-sleep wakeup.
 *
 * Extracts the alarm logic from fsm_hw_enter_deep_sleep().
 * Falls back to k_work_reschedule if RTC time is not valid.
 *
 * @param interval_ms  Wake interval in milliseconds.
 */
void lima_rtc_arm_wakeup(uint32_t interval_ms);

/**
 * @brief  Returns true if the RTC has a valid (non-fallback) time source.
 */
bool lima_rtc_is_valid(void);

#ifdef __cplusplus
}
#endif

#endif /* LIMA_RTC_H */
