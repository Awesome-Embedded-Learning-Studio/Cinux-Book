/**
 * @file    addr.hpp
 * @brief   PhysAddr: a physical address that refuses to be an integer.
 *
 * A raw unsigned long can silently flow between addresses, sizes, and
 * page indices; PhysAddr makes the mixing a compile error. The numeric
 * escape is the one named field, so every crossing is visible as one.
 *
 * @author  Charliechen114514
 * @date    2026-10-02
 * @version 0.1
 * @since   0.1.0
 * @ingroup base_addr
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <stdint.h>

namespace cinux::base {

/**
 * @brief   A physical address carried as its own type.
 * @note    Zero doubles as the "no address" answer of allocators, because the
 *          low megabyte never holds an allocated page.
 * @since   0.1.0
 * @ingroup base_addr
 */
struct PhysAddr {
    unsigned long raw = 0;

    constexpr PhysAddr() = default;

    /** @brief         Adopts a numeric address; the one explicit conversion in. */
    constexpr explicit PhysAddr(unsigned long address) : raw(address) {}

    /**
     * @brief         Returns the address this many bytes higher.
     *
     * @param[in]     bytes   Distance to add.
     * @return        The advanced address.
     * @note          None
     * @warning       None
     * @throws        None
     * @since         0.1.0
     * @ingroup       base_addr
     */
    [[nodiscard]] constexpr PhysAddr offset(unsigned long bytes) const {
        return PhysAddr{raw + bytes};
    }

    constexpr bool operator==(const PhysAddr& other) const = default;
    constexpr bool operator!=(const PhysAddr& other) const = default;
    constexpr bool operator<(const PhysAddr& other) const { return raw < other.raw; }
    constexpr bool operator>(const PhysAddr& other) const { return raw > other.raw; }
    constexpr bool operator<=(const PhysAddr& other) const { return raw <= other.raw; }
    constexpr bool operator>=(const PhysAddr& other) const { return raw >= other.raw; }
};

}  // namespace cinux::base
