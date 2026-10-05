/**
 * @file    byte_order.hpp
 * @brief   Reading little-endian scalars out of byte blobs.
 *
 * On-disk and firmware formats — PSF2 font headers, partition tables,
 * filesystem superblocks — spell their multi-byte fields little-endian,
 * while a blob is just bytes. This header is the single home for that
 * spelling; consumers never hand-roll the shift-and-or chain again.
 *
 * @author  Charliechen114514
 * @date    2026-10-05
 * @version 0.1
 * @since   0.1.0
 * @ingroup base_bytes
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <stdint.h>

namespace cinux::base {

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

}  // namespace cinux::base
