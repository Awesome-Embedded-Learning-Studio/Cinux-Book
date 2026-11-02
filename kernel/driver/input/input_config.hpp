/**
 * @file    input_config.hpp
 * @brief   The evdev wire vocabulary: one record shape, the event
 *          words, and the grab question.
 *
 * The record mirrors Linux's struct input_event byte for byte on this
 * machine, because the asking side is a user program compiled against
 * its libc's linux/input.h. A key or pointer report ends with one
 * synchronization word, the same rhythm Linux's own devices keep, so a
 * consumer that batches buttons until a report closes works unchanged.
 *
 * @author  Charliechen114514
 * @date    2026-10-26
 * @version 0.1
 * @since   0.9.0
 * @ingroup kernel_driver
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <stdint.h>

#include "cinux/bit_ops/bitmask.hpp"

namespace cinux::driver {

/// One wire record: the time stamp, then type, code and value. The
/// stamp's two words are eight bytes wide on this machine, matching the
/// record Linux hands a 64-bit reader.
struct InputEventWire {
    uint64_t seconds;       ///< Whole seconds of the monotonic stamp.
    uint64_t microseconds;  ///< The stamp's sub-second tail.
    uint16_t type;          ///< The event's class word.
    uint16_t code;          ///< The event's member inside its class.
    int32_t  value;         ///< The event's payload word.
};

/// Bytes in one wire record, pinned by the shape above.
inline constexpr unsigned long kInputEventBytes = 24;

/// Key class shared by keyboard and pointer devices.
inline constexpr uint16_t kEvKey = 0x01;

/// Absolute-axis class, the pointer's coordinates as positions.
inline constexpr uint16_t kEvAbs = 0x03;

/// EVIOCGKEY base command; the caller supplies its bitmap length in the ioctl size field.
inline constexpr unsigned long long kEvdevGetKeys = 0x80004518;

/// The EVIOCGKEY(len) word mask: direction, type and number only, the
/// caller's length rides in the size field.
inline constexpr unsigned long long kEvdevGetKeysMask = 0xC000FFFFULL;

/// Where the caller's bitmap length lives inside an EVIOCGKEY(len) word.
inline constexpr base::bit::BitRange kEvdevGetKeysSize{.low = 16, .width = 14};

/// Every event code this stream tracks; the get-keys bitmap covers it
/// in one bit per code, eight codes per byte.
inline constexpr unsigned long kInputKeyCodeCount = 0x300;

/// One bit per tracked code, the bitmap EVIOCGKEY answers with.
[[nodiscard]] constexpr unsigned long InputKeyCodeBytes() {
    return kInputKeyCodeCount / 8;
}

/**
 * @brief   One absolute axis's facts, the EVIOCGABS answer's shape.
 * @note    The words mirror Linux's struct input_absinfo byte for byte on
 *          this machine, because the asking side reads them as that
 *          struct: the latest position first, then the axis's range.
 * @since   0.9.0
 * @ingroup kernel_driver
 */
struct InputAbsInfo {
    int32_t value;       ///< The axis's latest position.
    int32_t minimum;     ///< The position the axis bottoms out at.
    int32_t maximum;     ///< The position the axis tops out at.
    int32_t fuzz;        ///< Flat noise the axis tolerates, unused here.
    int32_t flat;        ///< Dead zone around the center, unused here.
    int32_t resolution;  ///< Units per millimeter, zero when unstated.
};

/// EVIOCGABS for the x axis: the axis's range and latest position.
inline constexpr unsigned long long kEvdevGetAbsX = 0x80184540;

/// EVIOCGABS for the y axis: the axis's range and latest position.
inline constexpr unsigned long long kEvdevGetAbsY = 0x80184541;

/// The grab question, EVIOCGRAB: whose input is this node's alone.
inline constexpr unsigned long long kEvdevGrab = 0x40044590;

/// Inode band for per-open event nodes minted above the seated devices;
/// the offset names the device's minor number, and stat rebuilds the
/// character device identity from that pair.
inline constexpr unsigned long kInputInoBase = 0x2000;

/// The event devices' character major, the number Linux's evdev keeps.
inline constexpr unsigned long kInputEventMajor = 13;

/// Records one stream keeps before the newest are refused; a dropped
/// burst is a missed gesture, never a lie about what arrived.
inline constexpr unsigned long kInputEventQueueCapacity = 64;

static_assert(sizeof(InputEventWire) == kInputEventBytes,
              "the wire record keeps Linux's x86-64 shape");

static_assert(sizeof(InputAbsInfo) == 24, "the axis facts keep Linux's shape");

}  // namespace cinux::driver
