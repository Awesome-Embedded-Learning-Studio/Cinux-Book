/**
 * @file    page.hpp
 * @brief   The page size of this machine.
 *
 * An architecture fact, not a policy: x86-64 pages are four kilobytes, and
 every page computation from PMM upward reads it from here.
 *
 * @author  Charliechen114514
 * @date    2026-10-02
 * @version 0.1
 * @since   0.1.0
 * @ingroup base_page
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "cinux/literal_types.hpp"

namespace cinux::arch::page {

using cinux::base::operator""_KiB;

/** @brief Page size of the x86-64 long mode this tree targets. */
inline constexpr unsigned long kSize = 4_KiB;

}  // namespace cinux::arch::page
