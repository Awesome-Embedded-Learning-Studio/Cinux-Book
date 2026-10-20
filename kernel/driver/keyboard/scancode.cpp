#include "kernel/driver/keyboard/scancode.hpp"

#include <stdint.h>

namespace cinux::driver {

char TranslateScancode(TranslatorState& state, uint8_t raw_byte) {
    bool const    kRelease = (raw_byte & kScancodeReleaseBit) != 0;
    uint8_t const kMake    = raw_byte & static_cast<uint8_t>(~kScancodeReleaseBit);
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

}  // namespace cinux::driver
