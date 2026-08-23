/**
 * @file core/abi_check.cpp
 * @brief Compile-time ABI self-check for the cinux::gui Host ABI
 *
 * Includes the ABI headers and static_asserts the cross-process layout
 * contracts (event-header size, payload packed-ness, the aggregate Host carries
 * the desktop extension + opaque ctx). Pure compile-time; no runtime effect.
 */
#include "event.hpp"
#include "event_payload.hpp"
#include "host.hpp"

using namespace cinux::gui;

/* EventHeader is 8 bytes (u16 + u16 + u8 + u8 + u16) -- a cross-process ABI
 * drift here is caught at compile time. */
static_assert(sizeof(EventHeader) == 8, "EventHeader must be 8 bytes (u16+u16+u8+u8+u16)");

/* Rect is 4x int32 with no padding -- the dirty-rect wire shape. */
static_assert(sizeof(Rect) == 16, "Rect must be 16 bytes (4x int32)");

static_assert(sizeof(HostCore) > 0, "HostCore must be non-empty");
static_assert(sizeof(HostDesktop) > 0, "HostDesktop must be non-empty");

/* The aggregate Host carries core + desktop pointer + ctx, so it must be
 * strictly larger than just the core table. */
static_assert(sizeof(Host) > sizeof(HostCore),
              "Host must carry core + desktop pointer + opaque ctx");

/* Typed payloads that follow the header. */
static_assert(sizeof(PointerPayload) == 18,
              "PointerPayload must be 18 bytes (kind + 4*int32 + buttons, packed)");
static_assert(sizeof(KeycodePayload) == 3,
              "KeycodePayload must be 3 bytes (ascii + scancode + modifiers, packed)");
