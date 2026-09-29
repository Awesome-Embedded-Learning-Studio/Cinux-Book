/**
 * @file    memory_region.hpp
 * @brief   Half-open memory regions with intersection testing.
 *
 * @author  Charliechen114514
 * @date    2026-09-27
 * @version 0.1
 * @since   0.1.0
 * @ingroup base_region
 * @copyright Copyright (c) 2026
 */

#pragma once

namespace cinux::base::memory {
/**
 * @brief         A half-open memory region [base, top).
 *
 * @note          None
 * @warning       None
 * @since         0.1.0
 * @ingroup       base_region
 */
struct MemoryRegion {
    using MemPtr_t = unsigned int;
    constexpr MemoryRegion(MemPtr_t region_base, MemPtr_t region_top)
        : base(region_base), top(region_top) {}

    /**
     * @brief         Reports whether two regions share at least one byte.
     *
     * @param[in]     other   Region to test against.
     * @return        true when the regions overlap.
     * @note          Half-open semantics: touching endpoints do not overlap.
     * @warning       None
     * @throws        None
     * @since         0.1.0
     * @ingroup       base_region
     */
    [[nodiscard]] consteval bool is_intersects(MemoryRegion other) const noexcept {
        MemPtr_t const kLowerAddress  = base > other.base ? base : other.base;
        MemPtr_t const kHigherAddress = top < other.top ? top : other.top;
        return kLowerAddress < kHigherAddress;
    }

    MemPtr_t base;  // Base, or bottom of a region
    MemPtr_t top;   // Top one
};
}  // namespace cinux::base::memory
