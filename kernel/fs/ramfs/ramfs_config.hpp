/**
 * @file    ramfs_config.hpp
 * @brief   Fact knobs of the ramfs backend.
 *
 * Private to the memory-backed backend: how its file buffers grow.
 * Name and path caps are floor-wide and stay in fs_config.hpp.
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
 * @brief   ramfs grows file buffers in chunks of this many bytes.
 * @note    Matches the page grain on purpose: one reallocation per page
 *          of growth instead of one per byte, so a megabyte written one
 *          byte at a time pays 256 reallocations, not a million.
 * @since   0.1.0
 * @ingroup kernel_fs
 */
inline constexpr unsigned long kRamfsGrowthAlign = 4096;

}  // namespace cinux::fs
