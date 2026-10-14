/**
 * @file    bit_ops.hpp
 * @brief   Named bit-level primitives over plain integers.
 *
 * @author  Charliechen114514
 * @date    2026-09-27
 * @version 0.1
 * @since   0.1.0
 * @ingroup base_bit_ops
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <type_traits>

namespace cinux::base::bit {
/**
 * @brief         Low nibble of any integer: the least significant 4 bits.
 *
 * @param[in]     value   Integer to slice.
 * @return        Bits 3..0 of value.
 * @note          None
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       base_bit_ops
 */
template <typename T>
constexpr T LowNibble(T value) {
    return static_cast<T>(static_cast<unsigned>(value) & 0xFU);
}

/**
 * @brief         High nibble of any integer: bits 7..4.
 *
 * @param[in]     value   Integer to slice.
 * @return        Bits 7..4 of value.
 * @note          None
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       base_bit_ops
 */
template <typename T>
constexpr T HighNibble(T value) {
    return static_cast<T>((static_cast<unsigned>(value) >> 4U) & 0xFU);
}

/**
 * @brief         Low byte of any integer: the least significant 8 bits.
 *
 * @param[in]     value   Integer to slice.
 * @return        Bits 7..0 of value.
 * @note          None
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       base_bit_ops
 */
template <typename T>
constexpr T LowByte(T value) {
    return static_cast<T>(static_cast<unsigned>(value) & 0xFFU);
}

/**
 * @brief         High byte of any integer: bits 15..8.
 *
 * @param[in]     value   Integer to slice.
 * @return        Bits 15..8 of value.
 * @note          None
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       base_bit_ops
 */
template <typename T>
constexpr T HighByte(T value) {
    return static_cast<T>((static_cast<unsigned>(value) >> 8U) & 0xFFU);
}

/**
 * @brief         The nibble starting at a given bit position.
 *
 * @param[in]     value   Integer to slice.
 * @param[in]     shift   Starting bit (0, 4, 8, ...).
 * @return        Four bits of value beginning at shift.
 * @note          None
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       base_bit_ops
 */
template <typename T>
constexpr T NibbleAt(T value, unsigned char shift) {
    return static_cast<T>((static_cast<unsigned>(value) >> shift) & 0xFU);
}

// Lets Learn Rust, Test Static locally :)

}  // namespace cinux::base::bit
