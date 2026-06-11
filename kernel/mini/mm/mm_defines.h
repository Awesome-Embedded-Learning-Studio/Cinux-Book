/**
 * @file kernel/mini/mm/mm_defines.h
 * @brief Memory Management Common Definitions
 *
 * Central header for memory-related utilities and literals.
 * Alignment and literal operators delegate to Cinux-Base.
 */

#pragma once

#include <stdint.h>

#include <cinux/numeric.hpp>

// Import memory literals (_KB, _MB, _GB, _TB)
#include "memory_literals.h"

namespace cinux::mini::mm {

// ============================================================
// Common Page Size Definitions
// ============================================================
constexpr uint64_t PAGE_SIZE_4K = 4_KB;  // 4096 bytes
constexpr uint64_t PAGE_SIZE_2M = 2_MB;  // 2097152 bytes
constexpr uint64_t PAGE_SIZE_1G = 1_GB;  // 1073741824 bytes

// ============================================================
// Memory Alignment Helpers — delegate to Cinux-Base
// ============================================================
using cinux::lib::align_up;
using cinux::lib::align_down;
using cinux::lib::is_aligned;

}  // namespace cinux::mini::mm
