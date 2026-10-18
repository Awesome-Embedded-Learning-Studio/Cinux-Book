/**
 * @file    hpet_layout.hpp
 * @brief   The ACPI HPET table's layout decode and the tick conversion.
 *
 * @author  Charliechen114514
 * @date    2026-10-18
 * @version 0.3.0
 * @since   0.3.0
 * @ingroup kernel_driver
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <cstdint>

#include "cinux/byte_order.hpp"

namespace cinux::driver::hpet {

/**
 * @brief   The table and block geometry: header (36), block ID (4),
 *          GAS (12), number/tick/attributes (4).
 * @since   0.3.0
 * @ingroup kernel_driver
 */
inline constexpr unsigned long kTableBytes = 56;
inline constexpr unsigned long kBlockBytes = 1024;

/**
 * @brief     Decode the GAS from table bytes, which need not be
 *            naturally aligned.
 *
 * @return    The 4 KiB-aligned block address, or zero for a short
 *            table, a non-memory GAS, or an unusable block address.
 * @since     0.3.0
 * @ingroup   kernel_driver
 */
constexpr uint64_t BlockAddress(const unsigned char* table, unsigned long bytes) {
    constexpr unsigned long kSpaceAt   = 40;
    constexpr unsigned long kAddressAt = 44;
    if (bytes < kTableBytes || table[kSpaceAt] != 0) {
        return 0;
    }
    const uint64_t kBlock = base::ReadWord64(table, kAddressAt);
    return kBlock % kBlockBytes == 0 ? kBlock : 0;
}

/**
 * @brief     The HPET 1.0a requirement on the counter's period.
 *
 * @return    True when the period is nonzero and no greater than
 *            100 ns in femtoseconds.
 * @since     0.3.0
 * @ingroup   kernel_driver
 */
constexpr bool ValidPeriod(uint64_t period_fs) {
    return period_fs != 0 && period_fs <= 100'000'000ULL;
}

/**
 * @brief     Divide before multiplying so intermediate femtoseconds
 *            cannot wrap at ~5 h.
 *
 * @return    The elapsed nanoseconds for the ticks at the period;
 *            the result fits for elapsed times shorter than
 *            uint64_t nanoseconds (~584 y).
 * @since     0.3.0
 * @ingroup   kernel_driver
 */
constexpr uint64_t Nanoseconds(uint64_t ticks, uint64_t period_fs) {
    constexpr uint64_t kFsPerNs = 1'000'000;
    return ((ticks / kFsPerNs) * period_fs) + (((ticks % kFsPerNs) * period_fs) / kFsPerNs);
}

}  // namespace cinux::driver::hpet
