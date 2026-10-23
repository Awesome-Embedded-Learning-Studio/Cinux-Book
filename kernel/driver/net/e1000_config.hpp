/**
 * @file    e1000_config.hpp
 * @brief   Capacity and identity words for the e1000 card.
 *
 * Ring depths, buffer sizes, the MMIO window, and the vendor and
 * device identities this card answers to. The window is eight pages
 * because the MAC registers reach past the first four — a size that
 * was once learned the hard way.
 *
 * @author  Charliechen114514
 * @date    2026-10-23
 * @version 0.1
 * @since   0.6.0
 * @ingroup kernel_driver
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <stdint.h>

namespace cinux::driver::net {

/**
 * @brief   MMIO window this card needs, BAR0 to the last MAC register.
 * @since   0.6.0
 * @ingroup kernel_driver
 */
inline constexpr unsigned long kE1000WindowBytes = 32UL * 1024UL;

/**
 * @brief   How many receive descriptors the ring holds.
 * @since   0.6.0
 * @ingroup kernel_driver
 */
inline constexpr unsigned long kE1000RxRingSize = 32;

/**
 * @brief   How many transmit descriptors the ring holds.
 * @since   0.6.0
 * @ingroup kernel_driver
 */
inline constexpr unsigned long kE1000TxRingSize = 8;

/**
 * @brief   Bytes behind one receive descriptor.
 * @since   0.6.0
 * @ingroup kernel_driver
 */
inline constexpr unsigned long kE1000RxBufferBytes = 2048;

/**
 * @brief   Bytes behind one transmit descriptor, header included.
 * @since   0.6.0
 * @ingroup kernel_driver
 */
inline constexpr unsigned long kE1000TxBufferBytes = 1536;

/**
 * @brief   Vendor identity of Intel, the card's maker.
 * @since   0.6.0
 * @ingroup kernel_driver
 */
inline constexpr uint16_t kE1000VendorId = 0x8086;

/**
 * @brief   Device identity of the 82540em QEMU presents.
 * @since   0.6.0
 * @ingroup kernel_driver
 */
inline constexpr uint16_t kE1000DeviceId = 0x100E;

}  // namespace cinux::driver::net
