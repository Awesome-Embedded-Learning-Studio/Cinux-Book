/**
 * @file bit_ops.hpp
 * @brief Bit manipulation primitives — count, rotate, single-bit ops, bit-field extract/insert.
 *
 * All functions are constexpr. Kernel builds may use __builtin_* for runtime performance;
 * these implementations serve compile-time use and as portable fallbacks.
 */

#ifndef CINUX_BIT_OPS_HPP
#define CINUX_BIT_OPS_HPP

#include <cstdint>

namespace cinux::lib {

// ============================================================
// Bit counting
// ============================================================

/** @brief Population count — number of set bits in v. */
constexpr int popcount(uint32_t v) {
    int c = 0;
    while (v) {
        c += v & 1;
        v >>= 1;
    }
    return c;
}

/** @overload popcount for 64-bit. */
constexpr int popcount(uint64_t v) {
    int c = 0;
    while (v) {
        c += static_cast<int>(v & 1);
        v >>= 1;
    }
    return c;
}

/** @brief Count leading zeros in v. Undefined for v == 0 (returns 64). */
constexpr int clz(uint32_t v) {
    if (v == 0) {
        return 32;
    }
    int n = 0;
    for (int i = 31; i >= 0; --i) {
        if ((v >> i) & 1) {
            break;
        }
        ++n;
    }
    return n;
}

/** @overload clz for 64-bit. */
constexpr int clz(uint64_t v) {
    if (v == 0) {
        return 64;
    }
    int n = 0;
    for (int i = 63; i >= 0; --i) {
        if ((v >> i) & 1) {
            break;
        }
        ++n;
    }
    return n;
}

/** @brief Count trailing zeros in v. Returns 64 for v == 0. */
constexpr int ctz(uint32_t v) {
    if (v == 0) {
        return 32;
    }
    int n = 0;
    while ((v & 1) == 0) {
        ++n;
        v >>= 1;
    }
    return n;
}

/** @overload ctz for 64-bit. */
constexpr int ctz(uint64_t v) {
    if (v == 0) {
        return 64;
    }
    int n = 0;
    while ((v & 1) == 0) {
        ++n;
        v >>= 1;
    }
    return n;
}

// ============================================================
// Rotation
// ============================================================

/** @brief Rotate left. */
constexpr uint64_t rotl(uint64_t v, int n) {
    int s = n & 63;
    if (s == 0) {
        return v;
    }
    return (v << s) | (v >> (64 - s));
}

/** @brief Rotate right. */
constexpr uint64_t rotr(uint64_t v, int n) {
    int s = n & 63;
    if (s == 0) {
        return v;
    }
    return (v >> s) | (v << (64 - s));
}

// ============================================================
// Single-bit operations
// ============================================================

/** @brief Create a mask with bit n set. */
constexpr uint64_t bit(int n) {
    return uint64_t{1} << n;
}

/** @brief Set bit n in v. */
constexpr uint64_t set_bit(uint64_t v, int n) {
    return v | bit(n);
}

/** @brief Clear bit n in v. */
constexpr uint64_t clear_bit(uint64_t v, int n) {
    return v & ~bit(n);
}

/** @brief Toggle bit n in v. */
constexpr uint64_t toggle_bit(uint64_t v, int n) {
    return v ^ bit(n);
}

/** @brief Test whether bit n is set in v. */
constexpr bool test_bit(uint64_t v, int n) {
    return (v & bit(n)) != 0;
}

// ============================================================
// Bit-field extract / insert (hardware register access)
// ============================================================

/**
 * @brief Extract bits [high:low] (inclusive) from v.
 *
 * @param v    Source value.
 * @param high High bit index (inclusive).
 * @param low  Low bit index (inclusive).
 * @return     The extracted field, right-justified.
 */
constexpr uint64_t extract_bits(uint64_t v, int high, int low) {
    int      width = high - low + 1;
    uint64_t mask  = (width >= 64) ? ~uint64_t{0} : (uint64_t{1} << width) - 1;
    return (v >> low) & mask;
}

/**
 * @brief Insert val into bits [high:low] (inclusive) of v.
 *
 * @param v    Target value.
 * @param high High bit index.
 * @param low  Low bit index.
 * @param val  Value to insert (only low bits used).
 * @return     v with the field replaced by val.
 */
constexpr uint64_t insert_bits(uint64_t v, int high, int low, uint64_t val) {
    int      width   = high - low + 1;
    uint64_t mask    = (width >= 64) ? ~uint64_t{0} : (uint64_t{1} << width) - 1;
    uint64_t cleared = v & ~(mask << low);
    return cleared | ((val & mask) << low);
}

}  // namespace cinux::lib

#endif  // CINUX_BIT_OPS_HPP
