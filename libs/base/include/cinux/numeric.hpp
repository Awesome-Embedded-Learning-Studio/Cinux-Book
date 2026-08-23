/**
 * @file numeric.hpp
 * @brief Compile-time numeric utilities — alignment, division, power-of-2, memory literals.
 *
 * All functions are constexpr and suitable for static_assert.
 */

#ifndef CINUX_NUMERIC_HPP
#define CINUX_NUMERIC_HPP

#include <cstdint>

namespace cinux::lib {

// ============================================================
// Alignment
// ============================================================

/** @brief Round addr up to the nearest multiple of align. */
constexpr uint64_t align_up(uint64_t addr, uint64_t align) {
    // Fast path for power-of-two alignment
    if (align != 0 && (align & (align - 1)) == 0) {
        return (addr + align - 1) & ~(align - 1);
    }
    return ((addr + align - 1) / align) * align;
}

/** @brief Round addr down to the nearest multiple of align. */
constexpr uint64_t align_down(uint64_t addr, uint64_t align) {
    if (align != 0 && (align & (align - 1)) == 0) {
        return addr & ~(align - 1);
    }
    return (addr / align) * align;
}

/** @brief Check if addr is aligned to align. */
constexpr bool is_aligned(uint64_t addr, uint64_t align) {
    return (addr % align) == 0;
}

// ============================================================
// Power of two
// ============================================================

/** @brief Check if v is a power of two (v > 0). */
constexpr bool is_power_of_two(uint64_t v) {
    return v != 0 && (v & (v - 1)) == 0;
}

/** @brief Round v up to the next power of two (or v if already one). */
constexpr uint64_t round_up_to_power_of_two(uint64_t v) {
    if (v == 0) {
        return 1;
    }
    if (is_power_of_two(v)) {
        return v;
    }
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v |= v >> 32;
    return v + 1;
}

// ============================================================
// Division
// ============================================================

/** @brief Ceiling division: ceil(a / b). b must be > 0. */
constexpr uint64_t div_ceil(uint64_t a, uint64_t b) {
    return (a + b - 1) / b;
}

// ============================================================
// Logarithm
// ============================================================

/** @brief Floor(log2(v)) for v > 0. Returns -1 for v == 0. */
constexpr int log2_int(uint64_t v) {
    if (v == 0) {
        return -1;
    }
    int r = 0;
    while (v >>= 1) {
        ++r;
    }
    return r;
}

// ============================================================
// Memory literals
// ============================================================

namespace literals {

/** @brief Convert KiB to bytes. */
constexpr uint64_t operator""_KB(unsigned long long v) {
    return v * 1024ULL;
}

/** @brief Convert MiB to bytes. */
constexpr uint64_t operator""_MB(unsigned long long v) {
    return v * 1024ULL * 1024ULL;
}

/** @brief Convert GiB to bytes. */
constexpr uint64_t operator""_GB(unsigned long long v) {
    return v * 1024ULL * 1024ULL * 1024ULL;
}

/** @brief Convert TiB to bytes. */
constexpr uint64_t operator""_TB(unsigned long long v) {
    return v * 1024ULL * 1024ULL * 1024ULL * 1024ULL;
}

}  // namespace literals

}  // namespace cinux::lib

#endif  // CINUX_NUMERIC_HPP
