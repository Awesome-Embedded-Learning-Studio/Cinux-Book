/**
 * @file crc32.hpp
 * @brief CRC32 checksum computation.
 *
 * Declaration only — implementation in src/crc32.cpp.
 * Lookup table lives in the .cpp to avoid per-TU instantiation.
 */

#ifndef CINUX_CRC32_HPP
#define CINUX_CRC32_HPP

#include <cstddef>
#include <cstdint>

namespace cinux::lib {

/**
 * @brief Compute CRC32 checksum of the given data.
 * @param data Pointer to input bytes.
 * @param len  Number of bytes.
 * @return CRC32 checksum.
 */
uint32_t crc32(const void* data, size_t len);

}  // namespace cinux::lib

#endif  // CINUX_CRC32_HPP
