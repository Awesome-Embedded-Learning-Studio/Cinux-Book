/**
 * @file    devfs_config.hpp
 * @brief   Capacity words for the device file system's table.
 *
 * One home for the numbers devfs is sized by. The count covers the
 * builtin pair, the console, the PTY master, and the room later
 * stations add — a USB keyboard, say — because every character device
 * the device model seats lands here too.
 *
 * @author  Charliechen114514
 * @date    2026-10-22
 * @version 0.1
 * @since   0.6.0
 * @ingroup kernel_fs
 * @copyright Copyright (c) 2026
 */

#pragma once

namespace cinux::fs {

/**
 * @brief   How many devices the table holds at once.
 * @since   0.6.0
 * @ingroup kernel_fs
 */
inline constexpr unsigned long kDevfsDeviceMax = 16;

}  // namespace cinux::fs
