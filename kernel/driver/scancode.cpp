#include "kernel/driver/scancode.hpp"

#include <stdint.h>

namespace cinux::driver {

char TranslateScancode(TranslatorState& state, uint8_t raw_byte) {
    if (state.extended) {
        state.extended = raw_byte == kScancodeExtendedLeader;
        return 0;
    }
    if (raw_byte == kScancodeExtendedLeader) {
        state.extended = true;
        return 0;
    }
    bool const    kRelease = (raw_byte & kScancodeReleaseBit) != 0;
    uint8_t const kMake    = raw_byte & static_cast<uint8_t>(~kScancodeReleaseBit);
    if (kMake == kScancodeShiftLeft || kMake == kScancodeShiftRight) {
        state.shift_held = !kRelease;
        return 0;
    }
    if (kRelease || kMake >= kScancodeGlyphs) {
        return 0;
    }
    return state.shift_held ? kScancodeUpper[kMake] : kScancodeLower[kMake];
}

}  // namespace cinux::driver
