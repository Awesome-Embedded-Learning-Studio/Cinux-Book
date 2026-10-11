/**
 * @file    block_config.hpp
 * @brief   Facts and knobs of the block layer.
 *
 * @author  Charliechen114514
 * @date    2026-10-08
 * @version 0.2
 * @since   0.1.0
 * @ingroup kernel_driver
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <stdint.h>

namespace cinux::driver {

/// @brief Bytes in one disk sector, this machine's block-transfer unit.
inline constexpr unsigned long kSectorBytes = 512;

/// @brief Requests the queue keeps room for; the rest wait on the lock.
inline constexpr uint16_t kBlockQueueDepth = 16;

/// @brief Slots the block cache keeps at most.
inline constexpr unsigned long kBlockCacheSlots = 8;

}  // namespace cinux::driver
