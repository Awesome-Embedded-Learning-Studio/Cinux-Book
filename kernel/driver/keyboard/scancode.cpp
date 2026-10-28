#include "kernel/driver/keyboard/scancode.hpp"

#include <stdint.h>

#include <algorithm>
#include <iterator>

#include "cinux/bit_ops/bitmask.hpp"

namespace cinux::driver {
namespace {
/// One extended-group make code and the event code it names.
struct ExtendedKeycode {
    uint8_t  make;
    uint16_t code;
};

/// The make-code field: the low seven bits of a set-1 byte.
constexpr base::bit::BitRange kScancodeMakeSegment{.low = 0, .width = 7};

/// Pause's multi-byte leader, which the translator ignores whole.
constexpr uint8_t kScancodePauseLeader = 0xE1;

/// Another vendor's multi-byte leader, ignored the same way.
constexpr uint8_t kScancodeBreakLeader = 0xE2;

/// The extended group's members: arrows, the right-hand modifiers, and
/// the keypad keys that share a code with their left-side kin. The set-1
/// main block needs no table — its make codes are the event codes.
constexpr ExtendedKeycode kScancodeExtendedKeycodes[] = {
    {.make = 0x1C, .code = 96},  {.make = 0x1D, .code = 97},  {.make = 0x35, .code = 98},
    {.make = 0x38, .code = 100}, {.make = 0x47, .code = 102}, {.make = 0x48, .code = 103},
    {.make = 0x49, .code = 104}, {.make = 0x4B, .code = 105}, {.make = 0x4D, .code = 106},
    {.make = 0x4F, .code = 107}, {.make = 0x50, .code = 108}, {.make = 0x51, .code = 109},
    {.make = 0x52, .code = 110}, {.make = 0x53, .code = 111}, {.make = 0x5B, .code = 125},
    {.make = 0x5C, .code = 126}, {.make = 0x5D, .code = 127},
};

}


char TranslateScancode(TranslatorState& state, uint8_t raw_byte) {
    bool const kRelease =
        base::bit::BitMask{raw_byte}.has(base::bit::BitMask<uint8_t>{kScancodeReleaseBit});
    uint8_t const kMake = base::bit::BitMask{raw_byte}.extract(kScancodeMakeSegment);
    if (state.extended) {
        if (kMake == kScancodeControl) {
            state.control_right = !kRelease;
        }
        state.extended = raw_byte == kScancodeExtendedLeader;
        return 0;
    }
    if (raw_byte == kScancodeExtendedLeader) {
        state.extended = true;
        return 0;
    }
    if (kMake == kScancodeControl) {
        state.control_left = !kRelease;
        return 0;
    }
    if (kMake == kScancodeShiftLeft || kMake == kScancodeShiftRight) {
        state.shift_held = !kRelease;
        return 0;
    }
    if (kRelease || kMake >= kScancodeGlyphs) {
        return 0;
    }
    const char kGlyph = state.shift_held ? kScancodeUpper[kMake] : kScancodeLower[kMake];
    if (state.control_left || state.control_right) {
        if (kGlyph >= 'a' && kGlyph <= 'z') {
            return static_cast<char>(kGlyph - 'a' + 1);
        }
        if (kGlyph >= '@' && kGlyph <= '_') {
            return static_cast<char>(kGlyph - '@');
        }
    }
    return kGlyph;
}

bool TranslateScancodeKeycode(KeycodeState& state, uint8_t raw_byte, KeycodeEvent& out) {
    if (state.extended) {
        state.extended = false;
        const bool kRelease =
            base::bit::BitMask{raw_byte}.has(base::bit::BitMask<uint8_t>{kScancodeReleaseBit});
        const uint8_t     kMake = base::bit::BitMask{raw_byte}.extract(kScancodeMakeSegment);
        const auto* const kMember =
            std::ranges::find(kScancodeExtendedKeycodes, kMake, &ExtendedKeycode::make);
        if (kMember != std::end(kScancodeExtendedKeycodes)) {
            out = KeycodeEvent{.code = kMember->code, .pressed = !kRelease};
            return true;
        }
        return false;
    }
    if (raw_byte == kScancodeExtendedLeader) {
        state.extended = true;
        return false;
    }
    if (raw_byte == kScancodePauseLeader || raw_byte == kScancodeBreakLeader) {
        return false;
    }
    const bool kRelease =
        base::bit::BitMask{raw_byte}.has(base::bit::BitMask<uint8_t>{kScancodeReleaseBit});
    const uint8_t kMake = base::bit::BitMask{raw_byte}.extract(kScancodeMakeSegment);
    if (kMake == 0 || kMake >= kScancodeGlyphs) {
        return false;
    }
    out = KeycodeEvent{.code = kMake, .pressed = !kRelease};
    return true;
}

}  // namespace cinux::driver
