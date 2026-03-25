/*
 * L.I.M.A. — Local Integrity Multi-modal Architecture
 * rtc.c — DS3231 RTC management, NVS epoch backup, tamper-epoch anchor
 *
 * Boot priority:
 *   1. DS3231 hardware (battery-backed, authoritative)
 *   2. NVS last-known epoch (MCU power loss survival)
 *   3. CONFIG_LIMA_PROVISION_UNIX_TIME (Kconfig bake-in, last resort)
 *
 * Tamper signal:
 *   |rtc_epoch − nvs_epoch| > LIMA_RTC_SKEW_THRESHOLD_S  →  LIMA_RTC_ERR_SKEW
 *   Caller (main.c) posts LIMA_EVT_TAMPER_DETECTED.
 *
 * Wall-clock anchor:
 *   boot_epoch   — unix seconds at lima_rtc_init() time
 *   boot_uptime  — k_uptime_get() at same instant
 *   lima_rtc_timestamp_ms() = (boot_epoch * 1000) + (k_uptime_get() - boot_uptime)
 *
 * Copyright (c) 2026 Justin Knox <justin@nullsec.systems>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/counter.h>
#include <zephyr/drivers/rtc/maxim_ds3231.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/notify.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/gpio.h>
#include "rtc.h"
#include "events.h"   /* lima_event_t, LIMA_EVT_RTC_WAKEUP */

LOG_MODULE_REGISTER(lima_rtc, CONFIG_LIMA_RTC_LOG_LEVEL);

/* ── Settings key ──────────────────────────────────────────────────────────
 * Stored under the Settings subsystem (CONFIG_SETTINGS_NVS=y).
 * settings_load() in main.c mounts the backend before lima_rtc_init().
 * No separate nvs_mount() needed — Settings owns the storage partition.
 */
#define LIMA_RTC_SETTINGS_KEY   "lima/epoch"

/* ── Private state ─────────────────────────────────────────────────────────*/

static const struct device *ds3231 = DEVICE_DT_GET(DT_NODELABEL(ds3231));

static bool rtc_valid = false;

/* Wall-clock anchor — set once in lima_rtc_init() */
static uint32_t boot_epoch    = 0;   /* unix seconds at boot         */
static int64_t  boot_uptime   = 0;   /* k_uptime_get() at same tick  */

/* Periodic NVS flush work */
static struct k_work_delayable nvs_flush_work;

/* Forward-declared for wakeup fallback */
extern int lima_post_event(const lima_event_t *evt);

/* RTC */
static const struct gpio_dt_spec rtc_int = GPIO_DT_SPEC_GET(DT_NODELABEL(ds3231), isw_gpios);



/* ── Settings helpers ───────────────────────────────────────────────────── */

static int rtc_settings_read_epoch(uint32_t *out)
{
    if (out == NULL) return -EINVAL;

    int rc = settings_runtime_get(LIMA_RTC_SETTINGS_KEY, out, sizeof(*out));
    if (rc <= 0) {
        LOG_WRN("[RTC] Settings: no saved epoch (rc=%d)", rc);
        return -ENOENT;
    }

    LOG_INF("[RTC] Settings: loaded epoch %u", *out);
    return 0;
}

static int rtc_settings_write_epoch(uint32_t epoch)
{
    int rc = settings_save_one(LIMA_RTC_SETTINGS_KEY, &epoch, sizeof(epoch));
    if (rc != 0) {
        LOG_ERR("[RTC] Settings: write failed (%d)", rc);
        return rc;
    }

    LOG_DBG("[RTC] Settings: flushed epoch %u", epoch);
    return 0;
}

/* ── Periodic NVS flush ─────────────────────────────────────────────────── */

static void nvs_flush_fn(struct k_work *work)
{
    ARG_UNUSED(work);

    uint32_t now = lima_rtc_get_epoch();
    rtc_settings_write_epoch(now);

    /* Reschedule — runs for lifetime of device */
    k_work_reschedule(&nvs_flush_work, K_MSEC(LIMA_RTC_NVS_FLUSH_MS));
}

/* ── DS3231 read/write helpers ──────────────────────────────────────────── */

/**
 * @brief Read current epoch from DS3231 hardware.
 * @param out  Receives unix seconds on success.
 * @return 0 on success, negative on failure.
 */
static int rtc_hw_read(uint32_t *out)
{
    if (!device_is_ready(ds3231)) {
        LOG_ERR("[RTC] DS3231 not ready");
        return -ENODEV;
    }

    uint32_t ticks = 0;
    // int rc = maxim_ds3231_get_syncpoint(ds3231, &sp);
    int rc = counter_get_value(ds3231, &ticks);
    if (rc != 0) {
        LOG_WRN("[RTC] DS3231 get_syncpoint failed (%d)", rc);
        return rc;
    }

    // *out = (uint32_t)sp.rtc.tv_sec;
    *out = ticks;
    LOG_INF("[RTC] DS3231 read: %u", *out);
    return 0;
}




/**
 * @brief Write epoch to DS3231 hardware.
 * @param epoch  Unix seconds to set.
 * @return 0 on success, negative on failure.
 */
static int rtc_hw_write(uint32_t epoch)
{
    if (!device_is_ready(ds3231)) {
        LOG_ERR("[RTC] DS3231 not ready");
        return -ENODEV;
    }

    struct sys_notify notify;
    sys_notify_init_spinwait(&notify);

    struct maxim_ds3231_syncpoint sp = {
        .rtc.tv_sec  = (time_t)epoch,
        .rtc.tv_nsec = 0,
        .syncclock   = maxim_ds3231_read_syncclock(ds3231),
    };

    int rc = maxim_ds3231_set(ds3231, &sp, &notify);
    if (rc != 0) {
        LOG_ERR("[RTC] DS3231 set failed (%d)", rc);
        return rc;
    }

    int result;
    while (sys_notify_fetch_result(&notify, &result) == -EAGAIN) {
        k_yield();
    }

    if (result != 0) {
        LOG_ERR("[RTC] DS3231 set completed with error (%d)", result);
        return result;
    }

    LOG_INF("[RTC] DS3231 written: %u", epoch);
    return 0;
}

/* ── Wakeup alarm callback ──────────────────────────────────────────────── */

static void rtc_alarm_cb(const struct device *dev,
                          uint8_t chan_id,
                          uint32_t ticks,
                          void *user_data)
{
    ARG_UNUSED(dev);
    ARG_UNUSED(chan_id);
    ARG_UNUSED(ticks);
    ARG_UNUSED(user_data);

    lima_event_t e = {
        .type         = LIMA_EVT_RTC_WAKEUP,
        .timestamp_ms = (uint32_t)lima_rtc_timestamp_ms(),
    };
    lima_post_event(&e);
}

static struct k_work_delayable rtc_fallback_work;

static void rtc_fallback_fn(struct k_work *work)
{
    ARG_UNUSED(work);

    lima_event_t e = {
        .type         = LIMA_EVT_RTC_WAKEUP,
        .timestamp_ms = (uint32_t)lima_rtc_timestamp_ms(),
    };
    lima_post_event(&e);
}

/* ── RTC ─────────────────────────────────────────────────────────────────── */

static struct gpio_callback rtc_gpio_cb_data;

static void rtc_gpio_cb(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    LOG_INF("[RTC] INT/SQW GPIO fired on P0.%d!", rtc_int.pin);
}

/* ── Human time ───────────────────────────────────────────────────────────── */

void lima_rtc_format_now(char *buf, size_t len)
{
    uint32_t epoch = lima_rtc_get_epoch();
    if (epoch == 0) {
        snprintk(buf, len, "epoch not set (uptime=%u ms)",
                 k_uptime_get_32());
        return;
    }
    /* Manual UTC breakdown — no POSIX dependency */
    uint32_t s = epoch % 60;   epoch /= 60;
    uint32_t m = epoch % 60;   epoch /= 60;
    uint32_t h = epoch % 24;   epoch /= 24;
    /* Days since 1970-01-01 → Gregorian */
    uint32_t days = epoch;
    uint32_t y = 1970;
    while (1) {
        uint32_t dy = ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 366 : 365;
        if (days < dy) break;
        days -= dy; y++;
    }
    static const uint8_t dim[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    uint32_t mo = 0;
    bool leap = ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0);
    while (1) {
        uint32_t dm = (mo == 1 && leap) ? 29 : dim[mo];
        if (days < dm) break;
        days -= dm; mo++;
    }
    snprintk(buf, len, "%04u-%02u-%02u %02u:%02u:%02u UTC",
             y, mo + 1, days + 1, h, m, s);
}

/* ── Public API ─────────────────────────────────────────────────────────── */

int lima_rtc_init(void)
{
    int      ret          = LIMA_RTC_OK;
    uint32_t hw_epoch     = 0;
    uint32_t nvs_epoch    = 0;
    bool     hw_ok        = false;
    bool     nvs_ok       = false;

    /* Wire INT/SQW GPIO interrupt — future tamper hook */
    gpio_pin_configure_dt(&rtc_int, GPIO_INPUT);
    gpio_pin_interrupt_configure_dt(&rtc_int, GPIO_INT_EDGE_FALLING);
    gpio_init_callback(&rtc_gpio_cb_data, rtc_gpio_cb, BIT(rtc_int.pin));
    gpio_add_callback(rtc_int.port, &rtc_gpio_cb_data);
    LOG_INF("[RTC] INT/SQW GPIO armed on P0.%d", rtc_int.pin);

    /* ── 1. Read epoch from Settings (already mounted by settings_load()) ── */
    nvs_ok = (rtc_settings_read_epoch(&nvs_epoch) == 0);

    /* ── 2. Read DS3231 ───────────────────────────────────────────────── */
    if (!device_is_ready(ds3231)) {
        LOG_ERR("[RTC] DS3231 not ready — skipping hardware read");
    } else {
        counter_cancel_channel_alarm(ds3231, 0);

        /* Clear alarm flags via raw I2C write to status register (0x0F) */
        const struct device *i2c = DEVICE_DT_GET(DT_NODELABEL(i2c0));
        uint8_t clear_buf[2] = {0x0F, 0x00};
        i2c_write(i2c, clear_buf, sizeof(clear_buf), 0x68);

        int start_ret = counter_start(ds3231);
        if (start_ret < 0 && start_ret != -EALREADY) {
            LOG_ERR("[RTC] counter_start failed (%d)", start_ret);
        } else {
            hw_ok = (rtc_hw_read(&hw_epoch) == 0);
        }
    }

    /* ── 3. Tamper comparison ─────────────────────────────────────────── */
    if (hw_ok && nvs_ok) {
        uint32_t skew = (hw_epoch > nvs_epoch)
                      ? (hw_epoch - nvs_epoch)
                      : (nvs_epoch - hw_epoch);

        if (skew > LIMA_RTC_SKEW_THRESHOLD_S) {
            LOG_WRN("[RTC] SKEW DETECTED: DS3231=%u NVS=%u delta=%u s (threshold=%u)",
                    hw_epoch, nvs_epoch, skew, LIMA_RTC_SKEW_THRESHOLD_S);
            ret = LIMA_RTC_ERR_SKEW;   /* caller posts LIMA_EVT_TAMPER_DETECTED */
        } else {
            LOG_INF("[RTC] epoch cross-check OK: DS3231=%u NVS=%u delta=%u s",
                    hw_epoch, nvs_epoch, skew);
        }
    }

    /* ── 4. Resolve epoch ─────────────────────────────────────────────── */
    if (hw_ok) {
        boot_epoch = hw_epoch;
        rtc_valid  = true;
        LOG_INF("[RTC] source: DS3231 hardware (%u)", boot_epoch);
    } else if (nvs_ok && nvs_epoch > 0) {
        boot_epoch = nvs_epoch;
        rtc_valid  = true;
        ret        = LIMA_RTC_ERR_FALLBACK;
        LOG_WRN("[RTC] source: NVS backup (%u) — DS3231 unavailable", boot_epoch);

        /* Best-effort: write NVS epoch back to DS3231 to resync hardware */
        rtc_hw_write(boot_epoch);
        rtc_settings_write_epoch(boot_epoch);
    } else if (CONFIG_LIMA_PROVISION_UNIX_TIME != 0) {
        boot_epoch = CONFIG_LIMA_PROVISION_UNIX_TIME;
        rtc_valid  = false;   /* Kconfig bake-in is not authoritative */
        ret        = LIMA_RTC_ERR_FALLBACK;
        LOG_WRN("[RTC] source: Kconfig bake-in (%u) — all live sources unavailable",
                boot_epoch);

        /* Still write to DS3231 and Settings so next boot has something */
        rtc_hw_write(boot_epoch);
        rtc_settings_write_epoch(boot_epoch);
    } else {
        LOG_ERR("[RTC] no time source available — timestamps will be zero-based");
        boot_epoch = 0;
        rtc_valid  = false;
        ret        = LIMA_RTC_ERR_HW;
    }

    /* ── 5. Anchor boot uptime ────────────────────────────────────────── */
    boot_uptime = k_uptime_get();

    /* ── 6. Flush resolved epoch to Settings immediately ─────────────────── */
    if (boot_epoch > 0) {
        rtc_settings_write_epoch(boot_epoch);
    }

    /* ── 7. Schedule periodic NVS flush ──────────────────────────────── */
    k_work_init_delayable(&nvs_flush_work, nvs_flush_fn);
    k_work_reschedule(&nvs_flush_work, K_MSEC(LIMA_RTC_NVS_FLUSH_MS));

    /* ── 8. Init fallback wakeup work ────────────────────────────────── */
    k_work_init_delayable(&rtc_fallback_work, rtc_fallback_fn);

    LOG_INF("[RTC] init complete — epoch=%u valid=%d ret=%d",
            boot_epoch, rtc_valid, ret);
    return ret;
}

uint32_t lima_rtc_get_epoch(void)
{
    int64_t elapsed_ms  = k_uptime_get() - boot_uptime;
    uint32_t elapsed_s  = (uint32_t)(elapsed_ms / 1000LL);
    return boot_epoch + elapsed_s;
}

uint64_t lima_rtc_timestamp_ms(void)
{
    int64_t elapsed_ms = k_uptime_get() - boot_uptime;
    return ((uint64_t)boot_epoch * 1000ULL) + (uint64_t)elapsed_ms;
}

int lima_rtc_set_epoch(uint32_t epoch)
{
    if (epoch == 0) return -EINVAL;

    /* Write hardware */
    int rc = rtc_hw_write(epoch);
    if (rc != 0) {
        LOG_ERR("[RTC] set_epoch: DS3231 write failed (%d)", rc);
        /* Still update software anchor — don't fail the whole call */
    }

    /* Re-anchor software clock */
    boot_epoch  = epoch;
    boot_uptime = k_uptime_get();
    rtc_valid   = true;

    /* Flush to Settings immediately */
    rtc_settings_write_epoch(epoch);

    LOG_INF("[RTC] epoch set: %u (gateway sync)", epoch);
    return 0;
}

void lima_rtc_flush(void)
{
    if (boot_epoch == 0) return;
    rtc_settings_write_epoch(lima_rtc_get_epoch());
}

void lima_rtc_arm_wakeup(uint32_t interval_ms)
{
    if (!device_is_ready(ds3231) || !rtc_valid) {
        LOG_WRN("[RTC] arm_wakeup: RTC not valid — using k_work fallback (%u ms)",
                interval_ms);
        k_work_reschedule(&rtc_fallback_work, K_MSEC(interval_ms));
        return;
    }

    /* Flush epoch before sleep so last-known time survives power loss */
    lima_rtc_flush();

    uint32_t now = 0;
    counter_get_value(ds3231, &now);

    uint32_t ticks = counter_us_to_ticks(ds3231,
                         (uint64_t)interval_ms * 1000ULL);

    LOG_INF("[RTC] arm_wakeup: now=%u ticks=%u alarm_at=%u",
            now, ticks, now + ticks);

    struct counter_alarm_cfg alarm = {
        .callback  = rtc_alarm_cb,
        .ticks     = now + ticks,
        .user_data = NULL,
        .flags     = COUNTER_ALARM_CFG_ABSOLUTE,
    };

    int rc = counter_set_channel_alarm(ds3231, 0, &alarm);
    if (rc != 0) {
        LOG_ERR("[RTC] alarm set failed (%d) — falling back to k_work", rc);
        k_work_reschedule(&rtc_fallback_work, K_MSEC(interval_ms));
        return;
    }

    LOG_INF("[RTC] deep-sleep alarm armed (interval=%u ms)", interval_ms);
}

bool lima_rtc_is_valid(void)
{
    return rtc_valid;
}
