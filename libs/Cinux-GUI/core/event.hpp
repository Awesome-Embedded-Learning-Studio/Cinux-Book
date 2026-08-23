/**
 * @file core/event.hpp
 * @brief cinux::gui fixed-width event header + variable tail
 *
 * A versioned header so an event-ABI mismatch cannot overrun: the header is the
 * contract, the tail is interpreted by (type, version, payload_len). Pure C++17;
 * PC hosts only.
 */
#pragma once

#include <stdint.h>

namespace cinux::gui {

inline constexpr uint16_t kEventMagic = 0x5253u; /* 'RS' -- endian/version sanity */
inline constexpr uint16_t kAbiVersion = 1u;

enum class EventCode : uint8_t {
    kPointer = 1, /* mouse / touch: abs + delta + buttons */
    kKeycode = 2, /* keyboard: scancode + ascii + modifiers */
    kEncoder = 3, /* rotary encoder: axis diff */
    kTouch   = 4, /* multi-slot touch */
};

/* Flag bits in EventHeader.flags. kEventFlagPressed is KeyCode-only (press vs
 * release); pointer press semantics live in PointerPayload.kind. */
inline constexpr uint8_t kEventFlagPressed      = 1u << 0;
inline constexpr uint8_t kEventFlagContinueRead = 1u << 1; /* more buffered; poll again */

/* Fixed-width event header (8 bytes, no padding). The variable-length payload
 * follows in memory; poll_event reads the header first, then interprets the
 * tail by (type, version, payload_len) -- a mismatch never reads past it. */
struct EventHeader {
    uint16_t  magic;       /* kEventMagic */
    uint16_t  version;     /* kAbiVersion */
    EventCode type;        /* discriminates the payload tail */
    uint8_t   flags;       /* kEventFlag* bitmask */
    uint16_t  payload_len; /* tail byte count */
};

}  // namespace cinux::gui
