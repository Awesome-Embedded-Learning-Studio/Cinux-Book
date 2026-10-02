/**
 * @file    pmm_config.hpp
 * @brief   PMM facts and knobs: the numbers the ledger is built from.
 *
 * Everything tunable or machine-specific about the physical memory manager
 * lives here — the low-memory reserve, the managed ceiling, the largest
 * contiguous order — so the announce header and the implementation both
 * read one config.
 *
 * @author  Charliechen114514
 * @date    2026-10-02
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_mm
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "cinux/bit_ops/bitmap.hpp"
#include "cinux/literal_types.hpp"
#include "kernel/arch/x86_64/page.hpp"

namespace cinux::mm {

using cinux::base::operator""_GiB;
using cinux::base::operator""_MiB;

/** @brief Largest contiguous order allocate_pages serves; order 9 spans one 2 MiB large page. */
inline constexpr int kMaxOrder = 9;

/** @brief Top of the firmware-and-boot megabyte the ledger always withholds. */
inline constexpr unsigned long kLowMemoryTop = 1_MiB;

/** @brief Highest physical address the ledger covers; RAM beyond is not managed. */
inline constexpr unsigned long kPmmMaxPhys = 16_GiB;

/** @brief Pages the bitmap spans, derived from the ceiling and the page size. */
inline constexpr unsigned long kPmmTotalPages = kPmmMaxPhys / cinux::arch::page::kSize;

/** @brief Storage words behind the page bitmap, sized at compile time. */
inline constexpr unsigned long kPmmBitmapWords =
    cinux::base::bit::Bitmap::words_for(kPmmTotalPages);

}  // namespace cinux::mm
