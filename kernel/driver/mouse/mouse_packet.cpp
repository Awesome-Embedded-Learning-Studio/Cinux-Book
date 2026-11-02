#include "kernel/driver/mouse/mouse_packet.hpp"

#include <stdint.h>

#include "cinux/bit_ops/bitmask.hpp"

namespace cinux::driver {

namespace {

/// The first byte's signature: bit three always one, overflow bits clear.
constexpr uint8_t kHeadAlwaysOne = 0x08;

/// Buttons live in the first byte's low three bits.
constexpr uint8_t kHeadLeft   = 0x01;
constexpr uint8_t kHeadRight  = 0x02;
constexpr uint8_t kHeadMiddle = 0x04;

/// The motion sign bits, one per axis.
constexpr uint8_t kHeadXSign = 0x10;
constexpr uint8_t kHeadYSign = 0x20;

/// The overflow pair, the head's two top bits.
constexpr base::bit::BitRange kHeadOverflowSegment{.low = 6, .width = 2};

bool head_has(uint8_t head, uint8_t bit) {
    return base::bit::BitMask{head}.has(base::bit::BitMask<uint8_t>{bit});
}
}

bool MousePacketDecoder::feed(uint8_t raw_byte, MouseSample& out) {
    const base::bit::BitMask<uint8_t> kHeadByte{raw_byte};
    if (seated_ == 0) {
        if (!kHeadByte.has(base::bit::BitMask<uint8_t>{kHeadAlwaysOne}) ||
            kHeadByte.extract(kHeadOverflowSegment) != 0) {
            return false;
        }
        bytes_[0] = raw_byte;
        seated_   = 1;
        return false;
    }
    bytes_[seated_] = raw_byte;
    ++seated_;
    if (seated_ < 3) {
        return false;
    }
    seated_              = 0;
    const uint8_t kHead  = bytes_[0];
    int32_t       step_x = bytes_[1];
    int32_t       step_y = bytes_[2];
    if (head_has(kHead, kHeadXSign)) {
        step_x -= 0x100;
    }
    if (head_has(kHead, kHeadYSign)) {
        step_y -= 0x100;
    }
    out        = MouseSample{};
    out.dx     = step_x;
    out.dy     = -step_y;
    out.left   = head_has(kHead, kHeadLeft);
    out.right  = head_has(kHead, kHeadRight);
    out.middle = head_has(kHead, kHeadMiddle);
    return true;
}

}  // namespace cinux::driver
