/**
 * @file    rtc.cpp
 * @brief   Port manners: select, settle, read, require agreement.
 *
 * @author  Charliechen114514
 * @date    2026-10-11
 * @version 0.2
 * @since   0.1.0
 * @ingroup kernel_driver
 * @copyright Copyright (c) 2026
 */

#include "kernel/driver/rtc/rtc.hpp"

#include <stdint.h>

#include "cinux/bit_ops/bitmask.hpp"
#include "kernel/driver/base/io.hpp"

namespace cinux::driver {

namespace {

using BitSet = base::bit::BitMask<uint8_t>;

constexpr uint16_t kAddressPort = 0x70;
constexpr uint16_t kDataPort    = 0x71;
constexpr uint16_t kPostPort    = 0x80;

constexpr uint8_t kRegSeconds = 0x00;
constexpr uint8_t kRegMinutes = 0x02;
constexpr uint8_t kRegHours   = 0x04;
constexpr uint8_t kRegDay     = 0x07;
constexpr uint8_t kRegMonth   = 0x08;
constexpr uint8_t kRegYear    = 0x09;
constexpr uint8_t kRegStatusA = 0x0A;
constexpr uint8_t kRegStatusB = 0x0B;
constexpr uint8_t kRegCentury = 0x32;

constexpr uint8_t kUpdateInProgressBit = 7;

uint8_t read_register(uint8_t reg) {
    OutB(PortWrite{.port = kAddressPort, .value = reg});
    OutB(PortWrite{.port = kPostPort, .value = 0});
    return InB(kDataPort);
}

bool same_raw(const RawClock& left, const RawClock& right) {
    return left.seconds == right.seconds && left.minutes == right.minutes &&
           left.hours == right.hours && left.day == right.day && left.month == right.month &&
           left.year == right.year && left.century == right.century;
}

RawClock read_raw_once() {
    while (BitSet{read_register(kRegStatusA)}.has(
        base::bit::MaskBit<uint8_t>(static_cast<unsigned char>(kUpdateInProgressBit)))) {
    }
    return RawClock{
        .seconds = read_register(kRegSeconds),
        .minutes = read_register(kRegMinutes),
        .hours   = read_register(kRegHours),
        .day     = read_register(kRegDay),
        .month   = read_register(kRegMonth),
        .year    = read_register(kRegYear),
        .century = read_register(kRegCentury),
    };
}

}  // namespace

unsigned long long Rtc::now_seconds() {
    RawClock raw = read_raw_once();
    for (RawClock again = read_raw_once(); !same_raw(raw, again); again = read_raw_once()) {
        raw = again;
    }
    return DecodeClock(raw, read_register(kRegStatusB));
}

}  // namespace cinux::driver
