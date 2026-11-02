/**
 * @file    scancode.hpp
 * @brief   Set 1 scancode vocabulary: the two glyph tables, the modifier
 *          state, and the translator's declaration.
 *
 * Set 1 is the grammar of choice because a release code is the make code
 * plus one bit, so press and release split on a mask. The 0xE0 extended
 * group is dropped whole: arrows and friends produce no event in this
 * station. The tables are inline constexpr data; the translator itself
 * lives in scancode.cpp.
 *
 * @author  Charliechen114514
 * @date    2026-10-05
 * @version 0.2
 * @since   0.1.0
 * @ingroup kernel_driver
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <stdint.h>

namespace cinux::driver {

/// Make codes the two glyph tables cover; anything above carries no
/// printable face.
inline constexpr uint8_t kScancodeGlyphs = 64;

/// Modifier state the scancode stream maintains.
struct TranslatorState {
    /// Either shift is held; both seats fold into one bit.
    bool shift_held = false;

    /// The previous byte was 0xE0; this byte ends the dropped group.
    bool extended = false;

    /// Left and right control modifiers, tracked independently.
    bool control_left  = false;
    bool control_right = false;
};

/// Glyph for each make code with no modifier held; zero codes no event.
inline constexpr char kScancodeLower[kScancodeGlyphs] = {
    0,   0,   '1', '2', '3', '4', '5', '6', '7',  '8', '9', '0',  '-',  '=', '\b', 0,
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o',  'p', '[', ']',  '\n', 0,   'a',  's',
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0,   '\\', 'z',  'x', 'c',  'v',
    'b', 'n', 'm', ',', '.', '/', 0,   '*', 0,    ' ', 0,   0,    0,    0,   0,    0,
};

/// Glyph for each make code with shift held; zero codes no event.
inline constexpr char kScancodeUpper[kScancodeGlyphs] = {
    0,   0,   '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_',  '+', '\b', 0,
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0,   'A',  'S',
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0,   '|', 'Z',  'X', 'C',  'V',
    'B', 'N', 'M', '<', '>', '?', 0,   '*', 0,   ' ', 0,   0,   0,    0,   0,    0,
};

/// The 0xE0 leader of the extended group.
inline constexpr uint8_t kScancodeExtendedLeader = 0xE0;

/// Make code of the left shift key.
inline constexpr uint8_t kScancodeShiftLeft = 0x2A;

/// Make code of the right shift key.
inline constexpr uint8_t kScancodeShiftRight = 0x36;

/// Control make code; an E0 leader selects the right-hand key.
inline constexpr uint8_t kScancodeControl = 0x1D;

/// The bit that splits release from press in set 1.
inline constexpr uint8_t kScancodeReleaseBit = 0x80;

/// One key's event face: the code the event stream publishes, and
/// whether the key went down or came up.
struct KeycodeEvent {
    uint16_t code;
    bool     pressed;
};

/// The event face's own extended-group tracker. The glyph translator
/// consumes the leader from the shared state, so the event face keeps
/// its own: one byte stream, two consumers, each swallowing its own
/// leader.
struct KeycodeState {
    bool extended = false;
};

/**
 * @brief         Translate one raw scancode byte against the modifier
 *                state: shift seats fold, the extended group drops
 *                whole, releases only clear modifiers, presses of
 *                printable keys return their glyph.
 *
 * @param[in,out] state   Modifier state, updated in place.
 * @param[in]     raw_byte   Byte read from the data port.
 * @return        The event's character, or zero when the byte carries
 *                no printable face.
 * @since         0.1.0
 * @ingroup       kernel_driver
 */
char TranslateScancode(TranslatorState& state, uint8_t raw_byte);

/**
 * @brief         Translate one raw scancode byte into its event face:
 *                the code an input stream publishes, and the key's
 *                direction.
 *
 * @param[in,out] state   The event face's own state, updated in place.
 * @param[in]     raw_byte   Byte read from the data port.
 * @param[out]    out        The code and direction, when the byte names
 *                          a key.
 * @return        True when out carries an event; the leader byte, a
 *                byte with no code, and anything the extended table
 *                leaves unnamed answer false.
 * @note          The main block's make codes are already the event
 *                codes, so only the extended group carries a table.
 * @since         0.9.0
 * @ingroup       kernel_driver
 */
[[nodiscard]] bool TranslateScancodeKeycode(KeycodeState& state, uint8_t raw_byte,
                                            KeycodeEvent& out);

}  // namespace cinux::driver
