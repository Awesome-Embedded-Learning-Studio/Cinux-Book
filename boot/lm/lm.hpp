/**
 * @file    lm.hpp
 * @brief   The long-mode activation contract: EFER bits, control-register
 *          bits, and the temporary page-table addresses.
 *
 * The two EFER constants sit next to each other on purpose: the historical
 * bug wrote the SVME bit where LME was meant, and the pairing keeps the
 * difference one glance wide. The table addresses derive from the layout
 * region, so the filler and the switch read one legislated fact instead
 * of three independent numbers.
 *
 * @author  Charliechen114514
 * @date    2026-10-01
 * @version 0.1
 * @since   0.1.0
 * @ingroup boot_lm
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "layout.hpp"

namespace cinux::boot::lm {

/// Extended Feature Enable Register, the MSR that gates long mode.
inline constexpr unsigned int kMsrEfer = 0xC0000080;

/// EFER bit 8: Long Mode Enable. Not 0x1000, which is SVME.
inline constexpr unsigned int kEferLme = 0x100;

/// EFER bit 12: AMD Secure Virtual Machine; adjacent to LME, unrelated to it.
inline constexpr unsigned int kEferSvme = 0x1000;

/// CR4 bit 5: Physical Address Extension, a long-mode prerequisite.
inline constexpr unsigned int kCr4Pae = 0x20;

/// CR0 bit 31: paging enable, the step that actually activates long mode.
inline constexpr unsigned long kCr0Pg = 0x80000000;

/// PML4 table address, the root the switch loads into CR3.
inline constexpr unsigned long kPml4Phys = kPageTables.base;

/// Page-directory-pointer table address.
inline constexpr unsigned long kPdptPhys = kPml4Phys + 0x1000;

/// Page-directory address, holding the 2MB large-page entries.
inline constexpr unsigned long kPdPhys = kPml4Phys + 0x2000;

static_assert(kEferLme != kEferSvme);
static_assert(kPml4Phys % 0x1000 == 0);
static_assert(kPdPhys + 0x1000 <= kPageTables.top);

}  // namespace cinux::boot::lm
