/**
 * @file    boot_port.hpp
 * @brief   I/O port addresses used by the early boot path.
 *
 * @author  Charliechen114514
 * @version 0.1
 * @date    2026-09-27
 * @since   0.1.0
 * @ingroup boot_early
 * @copyright Copyright (c) 2026
 */

#pragma once

namespace cinux::boot::serial {
/// Bochs/QEMU debug console: bytes sent to this port land in the debug
/// log instead of touching real hardware.
inline constexpr unsigned short kDebugconPort = 0xE9;
}  // namespace cinux::boot::serial
