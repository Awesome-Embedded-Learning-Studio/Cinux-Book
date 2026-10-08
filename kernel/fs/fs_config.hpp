/**
 * @file    fs_config.hpp
 * @brief   Fact knobs shared across the file-system floor.
 *
 * The caps every backend and the mount table must agree on live here:
 * one name cap, one path cap, one table width. Knobs private to a
 * backend live in that backend's own directory, next to its code.
 *
 * @author  Charliechen114514
 * @date    2026-10-07
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_fs
 * @copyright Copyright (c) 2026
 */

#pragma once

namespace cinux::fs {

/**
 * @brief   Longest entry name the layer carries, NUL included.
 * @note    Shared by backend node names and the FsDirent handed to
 *          callers, so one cap governs the whole floor. Ext2 raises this
 *          the day it arrives, in this file, in one place.
 * @since   0.1.0
 * @ingroup kernel_fs
 */
inline constexpr unsigned long kFsNameMax = 32;

/**
 * @brief   Longest path and mount prefix the layer carries, NUL included.
 * @since   0.1.0
 * @ingroup kernel_fs
 */
inline constexpr unsigned long kFsPathMax = 96;

/**
 * @brief   How many mount points the table holds at once.
 * @since   0.1.0
 * @ingroup kernel_fs
 */
inline constexpr unsigned long kMountMax = 8;

}  // namespace cinux::fs
