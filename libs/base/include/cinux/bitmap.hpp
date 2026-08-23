/**
 * @file bitmap.hpp
 * @brief Fixed-size bitmap — for buddy allocators, page tracking, CPU masks.
 */

#ifndef CINUX_BITMAP_HPP
#define CINUX_BITMAP_HPP

#include <cinux/bit_ops.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace cinux::lib {

/**
 * @brief Fixed-size bitmap backed by an array of uint64_t words.
 *
 * @tparam N  Number of bits.
 *
 * Bit i resides in words_[i / 64] at position i % 64.
 */
template <size_t N>
class BitMap {
public:
    constexpr BitMap() = default;

    // ============================================================
    // Single-bit operations
    // ============================================================

    constexpr void set(size_t pos) {
        if (pos < N) {
            words_[pos / 64] |= (uint64_t{1} << (pos % 64));
        }
    }

    constexpr void clear(size_t pos) {
        if (pos < N) {
            words_[pos / 64] &= ~(uint64_t{1} << (pos % 64));
        }
    }

    constexpr void toggle(size_t pos) {
        if (pos < N) {
            words_[pos / 64] ^= (uint64_t{1} << (pos % 64));
        }
    }

    constexpr bool test(size_t pos) const {
        if (pos >= N) {
            return false;
        }
        return (words_[pos / 64] & (uint64_t{1} << (pos % 64))) != 0;
    }

    // ============================================================
    // Bulk operations
    // ============================================================

    void set_all() {
        memset(words_, 0xFF, sizeof(words_));
        // Clear bits beyond N in the last word
        constexpr size_t kTailBits = N % 64;
        if constexpr (kTailBits != 0) {
            words_[kWords - 1] &= (uint64_t{1} << kTailBits) - 1;
        }
    }
    void clear_all() { memset(words_, 0, sizeof(words_)); }

    constexpr void set_range(size_t begin, size_t end) {
        for (size_t i = begin; i < end && i < N; ++i) {
            set(i);
        }
    }

    constexpr void clear_range(size_t begin, size_t end) {
        for (size_t i = begin; i < end && i < N; ++i) {
            clear(i);
        }
    }

    // ============================================================
    // Queries
    // ============================================================

    constexpr size_t size() const { return N; }

    constexpr size_t count_set() const {
        size_t count = 0;
        for (size_t w = 0; w < kWords; ++w) {
            count += static_cast<size_t>(popcount(words_[w]));
        }
        return count;
    }

    constexpr size_t count_clear() const { return N - count_set(); }

    constexpr size_t find_first_set() const {
        for (size_t i = 0; i < N; ++i) {
            if (test(i)) {
                return i;
            }
        }
        return N;
    }

    constexpr size_t find_first_clear() const {
        for (size_t i = 0; i < N; ++i) {
            if (!test(i)) {
                return i;
            }
        }
        return N;
    }

    constexpr size_t find_next_clear(size_t pos) const {
        for (size_t i = pos; i < N; ++i) {
            if (!test(i)) {
                return i;
            }
        }
        return N;
    }

    // ============================================================
    // Data access
    // ============================================================

    constexpr const uint64_t* data() const { return words_; }
    constexpr size_t          data_size() const { return sizeof(words_); }

private:
    static constexpr size_t kWords = (N + 63) / 64;
    uint64_t                words_[kWords]{};
};

}  // namespace cinux::lib

#endif  // CINUX_BITMAP_HPP
