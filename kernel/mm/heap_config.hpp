/**
 * @file    heap_config.hpp
 * @brief   Heap facts and knobs: the numbers the kernel heap is built from.
 *
 * Everything tunable about the heap's birth size and guard spacing lives
 * here so the bring-up, the tests, and the chapter all read one config.
 *
 * @author  Charliechen114514
 * @date    2026-10-05
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_mm
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "cinux/literal_types.hpp"

namespace cinux::mm {

using cinux::base::operator""_KiB;

/** @brief Pages mapped at bring-up, before the first growth ever runs. */
inline constexpr unsigned long kHeapInitialBytes = 64_KiB;

/** @brief Dead page fenced off below the first block, per the layout map. */
inline constexpr unsigned long kHeapLowerGuardBytes = 4_KiB;

}  // namespace cinux::mm
