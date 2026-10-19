/*
 * SPDX-FileCopyrightText: 2026 Yuzuki Tsuru
 * SPDX-License-Identifier: Apache-2.0
 *
 * The real time clock of the F101. The date is a day number (1 is the 1st of January 1970, the
 * counter holds 127 years) and the time of day is hours, minutes and seconds in one word. Both
 * registers are written through a slow path: a bit of the control register stays set while a write
 * is on its way to the 32 kHz domain, and a second write in that time is lost.
 */
#include <string.h>
#include <sys/time.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_private/startup_internal.h"
#include "soc/sun252i_f101_ll.h"
#include "f101_rtc.h"

static const char *TAG = "rtc";

#define RTC_BASE        0x03090000u
#define REG_LOSC_CTRL   0x00u
#define REG_AUTO_STA    0x04u
#define REG_YMD         0x10u
#define REG_HMS         0x14u
#define REG_HOSC_32K    0x60u

/* control register: the key in the upper half guards every write */
#define LOSC_KEY                0x16aa0000u
#define LOSC_SRC_EXTERNAL       (1u << 0)
#define LOSC_SRC_HOSC           (1u << 1)
#define LOSC_EXT_EN             (1u << 4)
#define LOSC_YMD_BUSY           (1u << 7)
#define LOSC_HMS_BUSY           (1u << 8)
#define LOSC_AUTO_SWITCH_EN     (1u << 14)
#define LOSC_AUTO_SWITCH_BYPASS (1u << 15)
#define AUTO_STA_EXT_OK         (1u << 2)
#define HOSC_32K_EN             (1u << 16)

#define YEAR_MIN 1970
#define YEAR_MAX 2097

static inline uint32_t rd(uint32_t off)
{
    return F101_REG32(RTC_BASE + off);
}

static inline void wr(uint32_t off, uint32_t val)
{
    F101_REG32(RTC_BASE + off) = val;
}

static bool is_leap(int y)
{
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

static const uint8_t month_len[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

static int days_in_month(int year, int mon)
{
    return month_len[mon] + (mon == 1 && is_leap(year) ? 1 : 0);
}

/* day number of a date, 1 for the 1st of January 1970; mon is 0..11 */
static uint32_t day_number(int year, int mon, int mday)
{
    uint32_t days = mday;

    for (int m = 0; m < mon; m++) {
        days += days_in_month(year, m);
    }
    for (int y = YEAR_MIN; y < year; y++) {
        days += is_leap(y) ? 366 : 365;
    }
    return days;
}

static void date_of_day(uint32_t day, struct tm *tm)
{
    int year = YEAR_MIN, mon = 0;
    uint32_t d = day;

    while (d > (uint32_t)(is_leap(year) ? 366 : 365)) {
        d -= is_leap(year) ? 366 : 365;
        year++;
    }
    tm->tm_yday = d - 1;
    while (d > (uint32_t)days_in_month(year, mon)) {
        d -= days_in_month(year, mon);
        mon++;
    }
    tm->tm_year = year - 1900;
    tm->tm_mon = mon;
    tm->tm_mday = d;
    tm->tm_wday = (day - 1 + 4) % 7;    /* the 1st of January 1970 was a Thursday */
}

static esp_err_t wait_clear(uint32_t mask)
{
    uint64_t end = f101_mtime_get() + 1000ull * (F101_MTIME_HZ / 1000);

    while (rd(REG_LOSC_CTRL) & mask) {
        if (f101_mtime_get() > end) {
            return ESP_ERR_TIMEOUT;
        }
        f101_delay_us(2000);
    }
    return ESP_OK;
}

esp_err_t f101_rtc_set_time(const struct tm *tm)
{
    int year;

    ESP_RETURN_ON_FALSE(tm, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    year = tm->tm_year + 1900;
    ESP_RETURN_ON_FALSE(year >= YEAR_MIN && year <= YEAR_MAX && tm->tm_mon >= 0 && tm->tm_mon <= 11 && tm->tm_mday >= 1 &&
                            tm->tm_mday <= days_in_month(year, tm->tm_mon) && tm->tm_hour >= 0 && tm->tm_hour <= 23 &&
                            tm->tm_min >= 0 && tm->tm_min <= 59 && tm->tm_sec >= 0 && tm->tm_sec <= 59,
                        ESP_ERR_INVALID_ARG, TAG, "time out of range");
    ESP_RETURN_ON_ERROR(wait_clear(LOSC_YMD_BUSY | LOSC_HMS_BUSY), TAG, "busy");
    wr(REG_HMS, (uint32_t)tm->tm_sec | (uint32_t)tm->tm_min << 8 | (uint32_t)tm->tm_hour << 16);
    ESP_RETURN_ON_ERROR(wait_clear(LOSC_HMS_BUSY), TAG, "busy");
    wr(REG_YMD, day_number(year, tm->tm_mon, tm->tm_mday));
    return wait_clear(LOSC_YMD_BUSY | LOSC_HMS_BUSY);
}

esp_err_t f101_rtc_get_time(struct tm *tm)
{
    uint32_t day, hms;

    ESP_RETURN_ON_FALSE(tm, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    /* the two registers belong to different domains: read until both are stable */
    do {
        day = rd(REG_YMD);
        hms = rd(REG_HMS);
    } while (day != rd(REG_YMD) || hms != rd(REG_HMS));
    if (day == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(tm, 0, sizeof(*tm));
    date_of_day(day, tm);
    tm->tm_sec = hms & 0x3f;
    tm->tm_min = (hms >> 8) & 0x3f;
    tm->tm_hour = (hms >> 16) & 0x1f;
    tm->tm_isdst = -1;
    return ESP_OK;
}

/* seconds since the epoch of a calendar time without a time zone */
static time_t to_epoch(const struct tm *tm)
{
    return (time_t)(day_number(tm->tm_year + 1900, tm->tm_mon, tm->tm_mday) - 1) * 86400 + tm->tm_hour * 3600 +
           tm->tm_min * 60 + tm->tm_sec;
}

esp_err_t f101_rtc_save_system_time(void)
{
    time_t now = time(NULL);
    struct tm tm;

    ESP_RETURN_ON_FALSE(gmtime_r(&now, &tm), ESP_FAIL, TAG, "time");
    return f101_rtc_set_time(&tm);
}

static void clock_source_setup(void)
{
    uint32_t v;

#ifdef CONFIG_ESP_RTC_F101_CLOCK_HOSC
    /*
     * The seconds are counted from the 24 MHz main crystal divided by 750: right to the accuracy of
     * the main crystal. The automatic switch back to the on-chip oscillator is off, the crystal
     * driver off, the divider on and the counter switched to it.
     */
    v = (rd(REG_LOSC_CTRL) & 0xffffu & ~LOSC_AUTO_SWITCH_EN) | LOSC_AUTO_SWITCH_BYPASS | LOSC_SRC_EXTERNAL;
    wr(REG_LOSC_CTRL, v | LOSC_KEY);
    v = rd(REG_LOSC_CTRL) & 0xffffu & ~LOSC_EXT_EN;
    wr(REG_LOSC_CTRL, v | LOSC_KEY);
    wr(REG_HOSC_32K, rd(REG_HOSC_32K) | HOSC_32K_EN);
    v = rd(REG_LOSC_CTRL) & 0xffffu;
    wr(REG_LOSC_CTRL, v | LOSC_SRC_HOSC | LOSC_KEY);
#endif
    ESP_LOGD(TAG, "control %08x, source status %08x (crystal %s)", (unsigned)rd(REG_LOSC_CTRL), (unsigned)rd(REG_AUTO_STA),
             (rd(REG_AUTO_STA) & AUTO_STA_EXT_OK) ? "ok" : "not running");
}

/* the system time starts from the clock when it has been set */
ESP_SYSTEM_INIT_FN(init_rtc, CORE, BIT(0), 150)
{
    struct tm tm;

    clock_source_setup();
    if (f101_rtc_get_time(&tm) == ESP_OK) {
        struct timeval tv = { .tv_sec = to_epoch(&tm), .tv_usec = 0 };

        settimeofday(&tv, NULL);
        ESP_EARLY_LOGI(TAG, "%04d-%02d-%02d %02d:%02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                       tm.tm_min, tm.tm_sec);
    }
    return ESP_OK;
}
