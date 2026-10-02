/**
 * @file    bitmap.hpp
 * @brief   A caller-backed bitmap over any number of bits.
 *
 * One flat word array does the bookkeeping; the storage belongs to the
 * owner, so base stays allocation-free. Single bits ride the BitMask
 * vocabulary — positions turn into masks in exactly one place.
 *
 * @author  Charliechen114514
 * @date    2026-10-02
 * @version 0.1
 * @since   0.1.0
 * @ingroup base_bit_ops
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <stdint.h>

#include "cinux/bit_ops/bitmask.hpp"

namespace cinux::base::bit {

/** @brief Sentinel returned when no run of the wanted length exists. */
inline constexpr unsigned long kInvalidIndex = ~0UL;

/** @brief Storage word type of every bitmap. */
using WordMask = BitMask<unsigned long>;

/**
 * @brief         The one-bit field covering a position.
 *
 * @param[in]     position   Bit index, zero-based from the least significant.
 * @return        A width-one BitRange at that position.
 * @note          None
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       base_bit_ops
 */
constexpr BitRange BitAt(unsigned char position) {
    return {.low = position, .width = 1};
}

/**
 * @brief   A set of bits over caller-provided words.
 * @note    Capacity counts bits, not words; the tail word is masked so
 *          whole-capacity operations stay honest at the edge.
 * @since   0.1.0
 * @ingroup base_bit_ops
 */
class Bitmap {
public:
    /**
     * @brief         Words needed to hold a capacity of bits.
     *
     * @param[in]     bit_capacity   Bit count to cover.
     * @return        (bit_capacity + 63) / 64; zero capacity needs zero words.
     * @note          None
     * @warning       None
     * @throws        None
     * @since         0.1.0
     * @ingroup       base_bit_ops
     */
    static constexpr unsigned long words_for(unsigned long bit_capacity) {
        return (bit_capacity + 63) / 64;
    }

    /**
     * @brief         Binds storage and starts every bit clear.
     *
     * @param[in]     words         Caller-owned word array, at least
     *                              words_for(bit_capacity) entries.
     * @param[in]     bit_capacity  Number of bits the bitmap governs.
     * @return        None
     * @note          None
     * @warning       None
     * @throws        None
     * @since         0.1.0
     * @ingroup       base_bit_ops
     */
    void init(WordMask* words, unsigned long bit_capacity) {
        words_     = words;
        capacity_  = bit_capacity;
        count_set_ = 0;
        clear_all();
    }

    /** @brief         Marks one bit; already-set bits keep the count honest. */
    void set(unsigned long index) {
        WordMask& word = words_[index / 64];
        if (!word.has(MaskBit<unsigned long>(static_cast<unsigned char>(index % 64)))) {
            word.deposit(BitAt(static_cast<unsigned char>(index % 64)), 1);
            ++count_set_;
        }
    }

    /** @brief         Clears one bit; already-clear bits keep the count honest. */
    void clear(unsigned long index) {
        WordMask& word = words_[index / 64];
        if (word.has(MaskBit<unsigned long>(static_cast<unsigned char>(index % 64)))) {
            word.deposit(BitAt(static_cast<unsigned char>(index % 64)), 0);
            --count_set_;
        }
    }

    /** @brief         Reports whether one bit is set. */
    [[nodiscard]] bool test(unsigned long index) const {
        return words_[index / 64].has(
            MaskBit<unsigned long>(static_cast<unsigned char>(index % 64)));
    }

    /**
     * @brief         Marks count consecutive bits from base.
     *
     * @param[in]     base   First bit index.
     * @param[in]     count  Bits to mark.
     * @return        None
     * @note          None
     * @warning       None
     * @throws        None
     * @since         0.1.0
     * @ingroup       base_bit_ops
     */
    void set_range(unsigned long base, unsigned long count) {
        for (unsigned long index = base; index < base + count; ++index) {
            set(index);
        }
    }

    /**
     * @brief         Clears count consecutive bits from base.
     *
     * @param[in]     base   First bit index.
     * @param[in]     count  Bits to clear.
     * @return        None
     * @note          None
     * @warning       None
     * @throws        None
     * @since         0.1.0
     * @ingroup       base_bit_ops
     */
    void clear_range(unsigned long base, unsigned long count) {
        for (unsigned long index = base; index < base + count; ++index) {
            clear(index);
        }
    }

    /**
     * @brief         Sets every bit up to capacity.
     *
     * @return        None
     * @note          The tail word is masked to capacity, so bits past the
     *                end never pretend to exist.
     * @warning       None
     * @throws        None
     * @since         0.1.0
     * @ingroup       base_bit_ops
     */
    void set_all() {
        if (capacity_ == 0) {
            return;
        }
        unsigned long const kWords    = words_for(capacity_);
        unsigned long const kTailBits = static_cast<unsigned char>(capacity_ % 64);
        for (unsigned long word_index = 0; word_index < kWords; ++word_index) {
            words_[word_index] = WordMask{Ones<unsigned long>(64)};
        }
        if (kTailBits != 0) {
            words_[kWords - 1] =  // NOLINT(clang-analyzer-security.ArrayBound)
                WordMask{Ones<unsigned long>(kTailBits)};
        }
        count_set_ = capacity_;
    }

    /** @brief         Clears every bit up to capacity. */
    void clear_all() {
        if (capacity_ == 0) {
            return;
        }
        unsigned long const kWords = words_for(capacity_);
        for (unsigned long word_index = 0; word_index < kWords; ++word_index) {
            words_[word_index] = WordMask{0};
        }
        count_set_ = 0;
    }

    /** @brief         How many bits are set. */
    [[nodiscard]] unsigned long set_count() const { return count_set_; }

    /** @brief         How many bits are clear. */
    [[nodiscard]] unsigned long clear_count() const { return capacity_ - count_set_; }

    /**
     * @brief         Finds the first run of consecutive clear bits.
     *
     * @param[in]     length   Run length wanted; zero or over-capacity asks
     *                         fail immediately.
     * @return        Index of the run start, or kInvalidIndex when none fits.
     * @note          Words with no clear bit are skipped whole.
     * @warning       None
     * @throws        None
     * @since         0.1.0
     * @ingroup       base_bit_ops
     */
    [[nodiscard]] unsigned long find_clear_run(unsigned long length) const {
        if (length == 0 || length > capacity_) {
            return kInvalidIndex;
        }
        unsigned long       run    = 0;
        unsigned long const kWords = words_for(capacity_);
        for (unsigned long word_index = 0; word_index < kWords; ++word_index) {
            if (words_[word_index].raw == Ones<unsigned long>(64)) {
                run = 0;
                continue;
            }
            for (unsigned bit = 0; bit < 64; ++bit) {
                unsigned long const kIndex = (word_index * 64) + bit;
                if (kIndex >= capacity_) {
                    break;
                }
                if (test(kIndex)) {
                    run = 0;
                    continue;
                }
                if (++run == length) {
                    return kIndex + 1 - length;
                }
            }
        }
        return kInvalidIndex;
    }

private:
    WordMask*     words_     = nullptr;
    unsigned long capacity_  = 0;
    unsigned long count_set_ = 0;
};

}  // namespace cinux::base::bit
