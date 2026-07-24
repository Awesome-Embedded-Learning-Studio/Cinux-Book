/**
 * @file kernel/drivers/ps2/ps2.hpp
 * @brief Intel 8042 PS/2 controller constants.
 *
 * The 8042 is a single controller wired to BOTH the keyboard (port 1) and the
 * auxiliary / mouse device (port 2).  Formerly mouse.cpp and keyboard.cpp each
 * carried a private partial copy of these (Ps2Port / Ps2Cmd / Ps2Status) with
 * drifting member sets; centralised here so the one hardware interface has one
 * source of truth.
 *
 * Namespace: cinux::drivers (so the Ps2Port::DATA / Ps2Cmd::* references in
 * mouse.cpp / keyboard.cpp resolve unchanged).
 */

#pragma once

#include <stdint.h>

namespace cinux::drivers {

namespace Ps2Port {
constexpr uint16_t DATA    = 0x60;  ///< PS/2 data register (read/write)
constexpr uint16_t STATUS  = 0x64;  ///< PS/2 status register (read)
constexpr uint16_t COMMAND = 0x64;  ///< PS/2 controller command (write)
}  // namespace Ps2Port

namespace Ps2Cmd {
constexpr uint8_t READ_CONFIG   = 0x20;
constexpr uint8_t WRITE_CONFIG  = 0x60;
constexpr uint8_t DISABLE_PORT2 = 0xA7;
constexpr uint8_t ENABLE_PORT2  = 0xA8;             ///< enable second port (the aux/mouse device)
constexpr uint8_t ENABLE_AUX    = ENABLE_PORT2;     ///< mouse-side alias for the same command
constexpr uint8_t DISABLE_PORT1 = 0xAD;
constexpr uint8_t ENABLE_PORT1  = 0xAE;
constexpr uint8_t SELF_TEST     = 0xAA;
constexpr uint8_t WRITE_AUX     = 0xD4;             ///< send the NEXT byte to the aux device (mouse)
}  // namespace Ps2Cmd

namespace Ps2Status {
constexpr uint8_t OUTPUT_FULL = 0x01;
constexpr uint8_t INPUT_FULL  = 0x02;
}  // namespace Ps2Status

}  // namespace cinux::drivers
