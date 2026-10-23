/**
 * @file    byte_order.hpp
 * @brief   Reading and writing little-endian scalars in byte blobs.
 *
 * On-disk and firmware formats — PSF2 font headers, partition tables,
 * filesystem superblocks — spell their multi-byte fields little-endian,
 * while a blob is just bytes. This header is the single home for that
 * spelling; consumers never hand-roll the shift-and-or chain again,
 * on the reading side or the writing side.
 *
 * @author  Charliechen114514
 * @date    2026-10-05
 * @version 0.2
 * @since   0.1.0
 * @ingroup base_bytes
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <stdint.h>

namespace cinux::base {

/**
 * @brief         Read one little-endian 16-bit word out of a byte blob.
 *
 * @param[in]     data    Blob to read from.
 * @param[in]     offset  Byte offset of the word's first byte.
 * @return        The word at that offset.
 * @warning       Undefined when the two bytes leave the blob; callers
 *                own the bounds.
 * @since         0.2.0
 * @ingroup       base_bytes
 */
constexpr uint16_t ReadWord16(const unsigned char* data, uint32_t offset) {
    const auto kLow = static_cast<uint16_t>(
        data[offset]);  // NOLINT(clang-analyzer-optin.core.FixedAddressDereference)
    const auto kHigh = static_cast<uint16_t>(
        data[offset + 1]);  // NOLINT(clang-analyzer-optin.core.FixedAddressDereference)
    return static_cast<uint16_t>(kLow | static_cast<uint16_t>(kHigh << 8U));
}

/**
 * @brief         Read one little-endian 32-bit word out of a byte blob.
 *
 * @param[in]     data    Blob to read from.
 * @param[in]     offset  Byte offset of the word's first byte.
 * @return        The word at that offset.
 * @warning       Undefined when the four bytes leave the blob; callers
 *                own the bounds.
 * @since         0.1.0
 * @ingroup       base_bytes
 */
constexpr uint32_t ReadWord32(const unsigned char* data, uint32_t offset) {
    return static_cast<uint32_t>(data[offset]) | (static_cast<uint32_t>(data[offset + 1]) << 8U) |
           (static_cast<uint32_t>(data[offset + 2]) << 16U) |
           (static_cast<uint32_t>(data[offset + 3]) << 24U);
}

/**
 * @brief         Read one little-endian 64-bit word out of a byte blob.
 *
 * @param[in]     data    Blob to read from.
 * @param[in]     offset  Byte offset of the word's first byte.
 * @return        The word at that offset.
 * @warning       Undefined when the eight bytes leave the blob; callers
 *                own the bounds.
 * @since         0.2.0
 * @ingroup       base_bytes
 */
constexpr uint64_t ReadWord64(const unsigned char* data, uint32_t offset) {
    return static_cast<uint64_t>(ReadWord32(data, offset)) |
           (static_cast<uint64_t>(ReadWord32(data, offset + 4)) << 32U);
}

/**
 * @brief         Write one little-endian 16-bit word into a byte blob.
 *
 * @param[out]    data    Blob to write into.
 * @param[in]     offset  Byte offset of the word's first byte.
 * @param[in]     value   Word to spell out.
 * @warning       Undefined when the two bytes leave the blob; callers
 *                own the bounds.
 * @since         0.2.0
 * @ingroup       base_bytes
 */
constexpr void WriteWord16(unsigned char* data, uint32_t offset, uint16_t value) {
    data[offset]     = static_cast<unsigned char>(value);
    data[offset + 1] = static_cast<unsigned char>(value >> 8U);
}

/**
 * @brief         Write one little-endian 32-bit word into a byte blob.
 *
 * @param[out]    data    Blob to write into.
 * @param[in]     offset  Byte offset of the word's first byte.
 * @param[in]     value   Word to spell out.
 * @warning       Undefined when the four bytes leave the blob; callers
 *                own the bounds.
 * @since         0.2.0
 * @ingroup       base_bytes
 */
constexpr void WriteWord32(unsigned char* data, uint32_t offset, uint32_t value) {
    data[offset]     = static_cast<unsigned char>(value);
    data[offset + 1] = static_cast<unsigned char>(value >> 8U);
    data[offset + 2] = static_cast<unsigned char>(value >> 16U);
    data[offset + 3] = static_cast<unsigned char>(value >> 24U);
}

/**
 * @brief         Write one big-endian 16-bit word into a byte blob.
 *
 * @param[out]    data    Blob to write into.
 * @param[in]     offset  Byte offset of the word's first byte.
 * @param[in]     value   Word to spell out.
 * @warning       Undefined when the two bytes leave the blob; callers
 *                own the bounds.
 * @since         0.6.0
 * @ingroup       base_bytes
 */
constexpr void WriteWord16Be(unsigned char* data, uint32_t offset, uint16_t value) {
    data[offset]     = static_cast<unsigned char>(value >> 8U);
    data[offset + 1] = static_cast<unsigned char>(value);
}

/**
 * @brief         Write one big-endian 32-bit word into a byte blob.
 *
 * @param[out]    data    Blob to write into.
 * @param[in]     offset  Byte offset of the word's first byte.
 * @param[in]     value   Word to spell out.
 * @warning       Undefined when the four bytes leave the blob; callers
 *                own the bounds.
 * @since         0.6.0
 * @ingroup       base_bytes
 */
constexpr void WriteWord32Be(unsigned char* data, uint32_t offset, uint32_t value) {
    data[offset]     = static_cast<unsigned char>(value >> 24U);
    data[offset + 1] = static_cast<unsigned char>(value >> 16U);
    data[offset + 2] = static_cast<unsigned char>(value >> 8U);
    data[offset + 3] = static_cast<unsigned char>(value);
}

/**
 * @brief         Read one big-endian 16-bit word out of a byte blob.
 *
 * @param[in]     data    Blob to read from.
 * @param[in]     offset  Byte offset of the word's first byte.
 * @return        The word at that offset.
 * @warning       Undefined when the two bytes leave the blob; callers
 *                own the bounds.
 * @since         0.6.0
 * @ingroup       base_bytes
 */
constexpr uint16_t ReadWord16Be(const unsigned char* data, uint32_t offset) {
    return static_cast<uint16_t>((static_cast<unsigned>(data[offset]) << 8U) |
                                 static_cast<unsigned>(data[offset + 1]));
}

/**
 * @brief         Read one big-endian 32-bit word out of a byte blob.
 *
 * @param[in]     data    Blob to read from.
 * @param[in]     offset  Byte offset of the word's first byte.
 * @return        The word at that offset.
 * @warning       Undefined when the four bytes leave the blob; callers
 *                own the bounds.
 * @since         0.6.0
 * @ingroup       base_bytes
 */
constexpr uint32_t ReadWord32Be(const unsigned char* data, uint32_t offset) {
    return (static_cast<uint32_t>(data[offset]) << 24U) |
           (static_cast<uint32_t>(data[offset + 1]) << 16U) |
           (static_cast<uint32_t>(data[offset + 2]) << 8U) |
           static_cast<uint32_t>(data[offset + 3]);
}

}  // namespace cinux::base
