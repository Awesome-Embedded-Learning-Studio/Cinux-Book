/**
 * @file    usb_config.hpp
 * @brief   Capacity and shape words for the USB family.
 *
 * The controller's MMIO window is one generous span: all four register
 * regions sit inside it, and the exact bounds come from the capability
 * region at bring-up. Ring capacities are bookkeeping choices sized for
 * bring-up traffic, one page each with room to spare.
 *
 * @author  Charliechen114514
 * @date    2026-10-22
 * @version 0.1
 * @since   0.6.0
 * @ingroup kernel_driver
 * @copyright Copyright (c) 2026
 */

#pragma once

namespace cinux::driver::usb {

/// @brief The controller's MMIO window, one span for all four regions.
inline constexpr unsigned long kXhciWindowBytes = 0x10000;

/// @brief Command ring slots, the last held by the link TRB.
inline constexpr unsigned long kCommandRingTrbs = 16;

/// @brief Event ring segment slots, completions the controller buffers.
inline constexpr unsigned long kEventRingTrbs = 16;

/// @brief Endpoint ring slots, control and interrupt alike.
inline constexpr unsigned long kTransferRingTrbs = 8;

/// @brief Device slots this bring-up asks the controller to enable.
inline constexpr unsigned long kXhciSlotsEnabled = 8;

/// @brief Glyphs the keyboard queue keeps between reads.
inline constexpr unsigned long kUsbKeyQueueCapacity = 16;

}  // namespace cinux::driver::usb
