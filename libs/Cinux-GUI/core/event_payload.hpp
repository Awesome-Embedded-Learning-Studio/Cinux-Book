/**
 * @file core/event_payload.hpp
 * @brief cinux::gui typed payloads that follow EventHeader
 *
 * EventHeader is the fixed 8-byte prefix; this header defines the payload
 * layouts that follow it, keyed by header.type. PointerPayload / KeycodePayload
 * are packed for the cross-process wire layout (verified by abi_check). Pure
 * C++17; PC hosts only.
 */
#pragma once

#include <stdint.h>


namespace cinux::gui {

/* Pointer-event kind (PointerPayload.kind). The header kEventFlagPressed alone
 * cannot tell move / down / up apart. */
inline constexpr uint8_t kPointerKindMove = 0;
inline constexpr uint8_t kPointerKindDown = 1;
inline constexpr uint8_t kPointerKindUp   = 2;

/* Key-code modifier bits (KeycodePayload.modifiers). */
inline constexpr uint8_t kKeymodShift = 1u << 0;
inline constexpr uint8_t kKeymodCtrl  = 1u << 1;
inline constexpr uint8_t kKeymodAlt   = 1u << 2;

/* POINTER payload: follows the header when type == EventCode::kPointer.
 * 18 bytes (kind + 4*int32 + buttons). Packed -- the cross-process wire shape. */
struct __attribute__((packed)) PointerPayload {
    uint8_t kind;    /* kPointerKind* */
    int32_t x;       /* absolute cursor X (pixels) */
    int32_t y;       /* absolute cursor Y (pixels) */
    int32_t dx;      /* relative X since last pointer event */
    int32_t dy;      /* relative Y since last pointer event (positive = down) */
    uint8_t buttons; /* bitmask: bit0=left, bit1=right, bit2=middle */
};

/* KEYCODE payload: follows the header when type == EventCode::kKeycode.
 * 3 bytes (ascii + scancode + modifiers). */
struct __attribute__((packed)) KeycodePayload {
    char    ascii;     /* 0 if non-printable */
    uint8_t scancode;  /* raw scan code set 1 */
    uint8_t modifiers; /* kKeymod* bitmask */
};

}  // namespace cinux::gui
