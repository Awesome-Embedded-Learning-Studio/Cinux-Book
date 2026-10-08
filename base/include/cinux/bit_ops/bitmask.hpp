/**
 * @file    bitmask.hpp
 * @brief   A typed word carrying named bit flags and packed bit fields.
 *
 * Bit positions are the only thing hand-written against this vocabulary;
 * every mask value is derived from a position, so the position-to-value
 * translation never happens inside a human head. The word type is a
 * template parameter, which makes the width itself a type-system fact:
 * a 32-bit register flag cannot be OR-ed into a 64-bit page entry.
 *
 * @author  Charliechen114514
 * @date    2026-10-01
 * @version 0.1
 * @since   0.1.0
 * @ingroup base_bit_ops
 * @copyright Copyright (c) 2026
 */

#pragma once

namespace cinux::base::bit {

/**
 * @brief         Where a bit field lives inside a word: lowest bit and width.
 *
 * @note          Field values shift up by low on deposit and back down by
 *                low on extract, so the pair is its own inverse.
 * @since         0.1.0
 * @ingroup       base_bit_ops
 */
struct BitRange {
    unsigned char low;
    unsigned char width;
};

/**
 * @brief         One unsigned word with the named-bit vocabulary on top.
 *
 * @note          raw is the whole storage and stays the width of T in every
 *                compiling world, so an array of these is byte-compatible
 *                with the hardware view of the same table.
 * @since         0.1.0
 * @ingroup       base_bit_ops
 */
template <typename T>
struct BitMask {
    static_assert(T(-1) > T(0));

    T raw = 0;

    constexpr BitMask() = default;

    /**
     * @brief         Adopt a word value as-is.
     *
     * @param[in]     value   Raw bits to carry.
     * @since         0.1.0
     * @ingroup       base_bit_ops
     */
    constexpr explicit BitMask(T value) : raw{value} {}

    /**
     * @brief         Test whether any bit of mask is set.
     *
     * @param[in]     mask   Bits to look for.
     * @return        true when at least one listed bit is set.
     * @since         0.1.0
     * @ingroup       base_bit_ops
     */
    [[nodiscard]] constexpr bool has(BitMask mask) const { return (raw & mask.raw) != T{0}; }

    /**
     * @brief         Union of two masks of the same word width.
     *
     * @param[in]     other   Mask to OR in.
     * @return        A mask holding every set bit of both operands.
     * @since         0.1.0
     * @ingroup       base_bit_ops
     */
    constexpr BitMask operator|(BitMask other) const {
        return BitMask{static_cast<T>(raw | other.raw)};
    }

    /// Intersection of two masks of the same word width.
    constexpr BitMask operator&(BitMask other) const {
        return BitMask{static_cast<T>(raw & other.raw)};
    }

    /// Complement within the storage word width.
    constexpr BitMask operator~() const { return BitMask{static_cast<T>(~raw)}; }

    /**
     * @brief         Read a field: shift down by low, keep width bits.
     *
     * @param[in]     range   Field location inside the word.
     * @return        The field value, right-justified.
     * @since         0.1.0
     * @ingroup       base_bit_ops
     */
    [[nodiscard]] constexpr T extract(BitRange range) const {
        return static_cast<T>((raw >> range.low) & field_mask(range.width));
    }

    /**
     * @brief         Write a field: clear the span, then place the value.
     *
     * @param[in]     range   Field location inside the word.
     * @param[in]     value   Right-justified field value; bits beyond the
     *                        width are dropped.
     * @note          Neighbouring fields and flag bits outside the span
     *                        keep their values.
     * @since         0.1.0
     * @ingroup       base_bit_ops
     */
    constexpr void deposit(BitRange range, T value) {
        T const kPlaced = static_cast<T>(field_mask(range.width) << range.low);
        raw             = static_cast<T>((raw & ~kPlaced) | ((value << range.low) & kPlaced));
    }

private:
    /**
     * @brief         Ones across width bits, right-justified.
     *
     * @param[in]     width   Number of one bits, counted from bit zero.
     * @return        The width-wide field mask.
     * @since         0.1.0
     * @ingroup       base_bit_ops
     */
    static constexpr T field_mask(unsigned char width) {
        return static_cast<T>((T{1} << width) - T{1});
    }
};

/**
 * @brief         Single-bit mask at a position.
 *
 * @param[in]     position   Bit index, zero-based from the least
 *                           significant bit.
 * @return        A mask with exactly that bit set.
 * @note          The template parameter is the raw word type, not a
 *                BitMask alias.
 * @since         0.1.0
 * @ingroup       base_bit_ops
 */
template <typename T>
constexpr BitMask<T> MaskBit(unsigned char position) {
    return BitMask<T>{static_cast<T>(T{1} << position)};
}

template <typename T>
constexpr T Ones(unsigned char width) {
    if (width >= sizeof(T) * 8) {
        return static_cast<T>(~T{0});
    }
    return static_cast<T>((T{1} << width) - T{1});
}

}  // namespace cinux::base::bit
