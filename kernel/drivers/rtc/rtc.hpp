/**
 * @file kernel/drivers/rtc/rtc.hpp
 * @brief CMOS Real-Time Clock driver (F5-M4)
 *
 * Reads the MC146818-compatible CMOS RTC over I/O ports 0x70/0x71, decodes the
 * BCD date/time, converts it to a Unix epoch second count, and -- at boot -- pins
 * that as the wall-clock baseline.  sys_clock_gettime(CLOCK_REALTIME) then reports
 * the RTC's coarse boot second refined by HPET's continuous monotonic delta (the
 * "drift correction"); the RTC itself is not re-read per call (port I/O is slow,
 * and periodic re-sync needs an IRQ, which is deferred).
 *
 * Pure helpers (bcd_to_binary / days_from_civil / datetime_to_unix_seconds) are
 * inline here so the arithmetic is host-testable without the port-I/O path.
 *
 * Namespace: cinux::drivers
 */

#pragma once

#include <stdint.h>

namespace cinux::drivers {

// ============================================================
// CMOS RTC I/O ports and register indices
// ============================================================
constexpr uint16_t kRtcIndexPort = 0x70;  ///< write register index here (bit 7 = NMI off)
constexpr uint16_t kRtcDataPort  = 0x71;  ///< read/write the selected register here

constexpr uint8_t kRtcRegSeconds    = 0x00;  ///< BCD seconds
constexpr uint8_t kRtcRegMinutes    = 0x02;  ///< BCD minutes
constexpr uint8_t kRtcRegHours      = 0x04;  ///< BCD hours (bit 7 = PM in 12h mode)
constexpr uint8_t kRtcRegDayOfMonth = 0x07;  ///< BCD day of month
constexpr uint8_t kRtcRegMonth      = 0x08;  ///< BCD month
constexpr uint8_t kRtcRegYear       = 0x09;  ///< BCD year (2 digits)
constexpr uint8_t kRtcRegCentury    = 0x32;  ///< BCD century (e.g. 0x20 -> 2000s)
constexpr uint8_t kRtcRegStatusA    = 0x0A;  ///< bit 7 = UIP (update in progress)
constexpr uint8_t kRtcRegStatusB    = 0x0B;  ///< bit 1 = 24h, bit 2 = binary mode

/// Status A bit 7: update-in-progress (set just before the 1-Hz register latch).
constexpr uint8_t kRtcStatusAUpdateInProgress = 0x80;
/// Status B bit 1: 24-hour format.
constexpr uint8_t kRtcStatusB24Hour           = 0x02;
/// Status B bit 2: binary (not BCD) mode.
constexpr uint8_t kRtcStatusBBinary           = 0x04;

// ============================================================
// Pure helpers (host-testable)
// ============================================================

/// Decode a packed-BCD byte to binary (0x26 -> 26).
inline uint8_t bcd_to_binary(uint8_t bcd) {
    return static_cast<uint8_t>((bcd & 0x0F) + ((bcd >> 4) * 10));
}

/// Days since 1970-01-01 for a (year, month, day) in the proleptic Gregorian
/// calendar (Howard Hinnant's days_from_civil -- exact and overflow-safe).
inline int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t  era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);            // [0, 399]
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;  // [0, 365]
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;           // [0, 146096]
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

/// Wall-clock (UTC) to Unix seconds.  Callers feed decoded binary fields.
inline int64_t datetime_to_unix_seconds(int64_t year, unsigned month, unsigned day, unsigned hour,
                                        unsigned minute, unsigned second) {
    return days_from_civil(year, month, day) * 86400 + static_cast<int64_t>(hour) * 3600 +
           static_cast<int64_t>(minute) * 60 + second;
}

// ============================================================
// Decoded date/time
// ============================================================

/// Wall-clock instant decoded from the CMOS RTC (binary fields, 24h, full year).
struct DateTime {
    uint16_t year;
    uint8_t  month;
    uint8_t  day;
    uint8_t  hour;
    uint8_t  minute;
    uint8_t  second;
};

// ============================================================
// RTC driver
// ============================================================

/**
 * @brief CMOS RTC driver (single instance).
 *
 * init() reads the RTC once and caches the Unix epoch second count as the boot
 * wall-clock baseline.  read_datetime() does a fresh port-I/O read whenever a
 * caller wants the live time.  All methods are safe before init() (return a
 * zero / unavailable result).
 */
class RTC {
public:
    /// Read the RTC once and cache the boot epoch.  Idempotent.
    void init();

    /// Whether init() captured a sane (>1970) boot epoch.
    bool available() const { return available_; }

    /// Live read of the current wall clock (port I/O).
    DateTime read_datetime() const;

    /// Cached boot wall clock as Unix seconds (0 if not initialised).
    int64_t  boot_epoch_seconds() const { return boot_epoch_seconds_; }
    /// Same as nanoseconds, for adding to HPET's monotonic delta.
    uint64_t boot_epoch_ns() const {
        return static_cast<uint64_t>(boot_epoch_seconds_) * 1'000'000'000ULL;
    }

private:
    uint8_t  read_reg(uint8_t index) const;
    void     wait_for_update_window() const;
    DateTime sample_fields(bool binary_mode, bool mode_24h) const;

    int64_t boot_epoch_seconds_ = 0;
    bool    available_          = false;
};

/// Global RTC instance.
extern RTC g_rtc;

}  // namespace cinux::drivers
