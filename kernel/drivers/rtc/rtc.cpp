/**
 * @file kernel/drivers/rtc/rtc.cpp
 * @brief CMOS RTC driver implementation (F5-M4)
 *
 * Drives the MC146818-compatible RTC through I/O ports 0x70/0x71: select a CMOS
 * register by writing its index (with NMI held off via bit 7) to 0x70, then read
 * the byte from 0x71.  The date/time is BCD by default; Status B reports whether
 * the firmware chose binary mode or 12-hour mode instead.  A read is torn if it
 * straddles the 1-Hz register update, so we wait for UIP (Status A bit 7) to
 * clear and re-read until two consecutive samples agree.
 */

#include "rtc.hpp"

#include <stdint.h>

#include "kernel/arch/x86_64/io.hpp"
#include "kernel/lib/kprintf.hpp"

using cinux::io::io_inb;
using cinux::io::io_outb;
using cinux::lib::kprintf;

namespace cinux::drivers {

RTC g_rtc;

namespace {

/// Decode a CMOS field to binary honouring the firmware's format selection.
uint8_t decode_field(bool binary_mode, uint8_t raw) {
    return binary_mode ? raw : bcd_to_binary(raw);
}

bool same_datetime(const DateTime& a, const DateTime& b) {
    return a.second == b.second && a.minute == b.minute && a.hour == b.hour && a.day == b.day &&
           a.month == b.month && a.year == b.year;
}

}  // namespace

uint8_t RTC::read_reg(uint8_t index) const {
    // Bit 7 of the index write disables NMI for the duration of the CMOS access
    // (an NMI mid-read can corrupt the RTC).  No need to restore it: NMI stays
    // off, which is the safe default for a hobby kernel.
    io_outb(kRtcIndexPort, static_cast<uint8_t>(0x80u | index));
    return io_inb(kRtcDataPort);
}

void RTC::wait_for_update_window() const {
    // UIP is asserted ~244 us before the 1-Hz register latch; when clear there is
    // a safe window to read all fields.  The bound (not the bit) guarantees we
    // never hang -- a cli test kernel or a stuck UIP simply falls through.
    for (int i = 0; i < 256; ++i) {
        if ((read_reg(kRtcRegStatusA) & kRtcStatusAUpdateInProgress) == 0) {
            return;
        }
    }
}

DateTime RTC::sample_fields(bool binary_mode, bool mode_24h) const {
    DateTime      dt{};
    const uint8_t sec    = read_reg(kRtcRegSeconds);
    const uint8_t min    = read_reg(kRtcRegMinutes);
    uint8_t       hr_raw = read_reg(kRtcRegHours);
    const uint8_t day    = read_reg(kRtcRegDayOfMonth);
    const uint8_t mon    = read_reg(kRtcRegMonth);
    const uint8_t yr     = read_reg(kRtcRegYear);
    const uint8_t cent   = read_reg(kRtcRegCentury);

    dt.second = decode_field(binary_mode, sec);
    dt.minute = decode_field(binary_mode, min);

    // 12-hour mode keeps the PM flag in bit 7 of the raw hours byte; mask it off
    // before decoding and apply it after.
    bool pm = false;
    if (!mode_24h) {
        pm = (hr_raw & 0x80) != 0;
        hr_raw &= 0x7F;
    }
    uint8_t hour = decode_field(binary_mode, hr_raw);
    if (!mode_24h) {
        if (hour == 12) {
            hour = 0;  // 12 AM == 0
        }
        if (pm) {
            hour = static_cast<uint8_t>(hour + 12);  // 1-11 PM -> 13-23, 12 PM -> 12
        }
    }
    dt.hour  = hour;
    dt.day   = decode_field(binary_mode, day);
    dt.month = decode_field(binary_mode, mon);

    const uint8_t year2   = decode_field(binary_mode, yr);
    const uint8_t century = decode_field(binary_mode, cent);
    // Fall back to the 2000s if the firmware does not provide a century register.
    dt.year               = static_cast<uint16_t>((century > 0 ? century : 20) * 100 + year2);
    return dt;
}

DateTime RTC::read_datetime() const {
    const uint8_t regb        = read_reg(kRtcRegStatusB);
    const bool    binary_mode = (regb & kRtcStatusBBinary) != 0;
    const bool    mode_24h    = (regb & kRtcStatusB24Hour) != 0;

    // Read until two consecutive samples agree (the classic anti-tear technique).
    // Converges immediately: QEMU's update ticks once per second.
    DateTime dt{};
    DateTime last{};
    bool     have_last = false;
    for (int round = 0; round < 4; ++round) {
        wait_for_update_window();
        dt = sample_fields(binary_mode, mode_24h);
        if (have_last && same_datetime(last, dt)) {
            return dt;
        }
        last      = dt;
        have_last = true;
    }
    return dt;
}

void RTC::init() {
    if (available_) {
        return;  // idempotent
    }

    const DateTime dt = read_datetime();
    boot_epoch_seconds_ =
        datetime_to_unix_seconds(dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
    available_ = boot_epoch_seconds_ > 0;

    kprintf("[RTC] %04u-%02u-%02u %02u:%02u:%02u (epoch %ld)%s\n", dt.year, dt.month, dt.day,
            dt.hour, dt.minute, dt.second, static_cast<long>(boot_epoch_seconds_),
            available_ ? "" : " -- UNAVAILABLE");
}

}  // namespace cinux::drivers
