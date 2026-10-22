/**
 * @file    net_config.hpp
 * @brief   Capacity and shape words for the network device family.
 *
 * The frame length is the family's own fact — an Ethernet frame tops
 * out at 1518 bytes on the wire, and every device face sizes its
 * buffers by that word. The loopback's queue depth is a bookkeeping
 * choice: enough frames for a sender to run ahead of a reader by a
 * burst, small enough to see the full answer.
 *
 * @author  Charliechen114514
 * @date    2026-10-21
 * @version 0.1
 * @since   0.6.0
 * @ingroup kernel_driver
 * @copyright Copyright (c) 2026
 */

#pragma once

namespace cinux::driver::net {

/**
 * @brief   How many bytes one Ethernet address carries.
 * @since   0.6.0
 * @ingroup kernel_driver
 */
inline constexpr unsigned long kMacAddressBytes = 6;

/**
 * @brief   The largest frame any Ethernet device moves, header to crc.
 * @since   0.6.0
 * @ingroup kernel_driver
 */
inline constexpr unsigned long kMaxEthernetFrame = 1518;

/**
 * @brief   How many frames the loopback holds before sends refuse.
 * @since   0.6.0
 * @ingroup kernel_driver
 */
inline constexpr unsigned long kLoopbackQueueFrames = 4;

}  // namespace cinux::driver::net
