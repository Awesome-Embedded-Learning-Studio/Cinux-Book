/**
 * @file    block_config.hpp
 * @brief   Queue knobs of the block layer.
 *
 * @author  Charliechen114514
 * @date    2026-10-08
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_driver
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <stdint.h>

namespace cinux::driver {

/// @brief Requests the queue keeps room for; the rest wait on the lock.
inline constexpr uint16_t kBlockQueueDepth = 16;

}  // namespace cinux::driver
