/**
 * @file checksum.hpp
 * @brief Internet checksum (RFC 1071) — IP/TCP/UDP/ICMP validation.
 */

#ifndef CINUX_CHECKSUM_HPP
#define CINUX_CHECKSUM_HPP

#include <cinux/span.hpp>
#include <cstddef>
#include <cstdint>

namespace cinux::lib {

/** @brief Compute Internet checksum (RFC 1071) over raw bytes. */
uint16_t internet_checksum(const void* data, size_t len);

/** @brief Overload accepting ConstByteSpan. */
uint16_t internet_checksum(ConstByteSpan data);

/** @brief Verify checksum — re-computing over data with embedded checksum yields 0xFFFF if valid.
 */
bool verify_internet_checksum(const void* data, size_t len);

/** @brief Compute TCP/UDP pseudo-header partial sum. */
uint32_t pseudo_header_partial(uint32_t src_ip, uint32_t dst_ip, uint8_t protocol,
                               uint16_t payload_len);

/** @brief Fold a 32-bit partial sum into a 16-bit checksum. */
uint16_t finalize_checksum(uint32_t partial_sum);

}  // namespace cinux::lib

#endif  // CINUX_CHECKSUM_HPP
