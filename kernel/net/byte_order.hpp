/**
 * @file kernel/net/byte_order.hpp
 * @brief Host <-> network byte-order helpers.
 *
 * Cinux is little-endian; network order is big-endian, so htons/ntohs swap
 * the two bytes of a 16-bit value.  Kept OUT of net_types.hpp on purpose --
 * that header is ZERO-logic POD/constants by design (its own header comment
 * says so), and byte-order conversion is logic.  Tcp/Udp socket .cpp files
 * formerly each carried a private byte_swap16 copy.
 *
 * Namespace: cinux::net
 */

#pragma once

#include <stdint.h>

namespace cinux::net {

/// Swap the two bytes of a 16-bit value (its own inverse).
constexpr inline uint16_t byte_swap16(uint16_t v) {
    return static_cast<uint16_t>((v >> 8) | (v << 8));
}

constexpr inline uint16_t htons16(uint16_t v) { return byte_swap16(v); }  ///< host -> network
constexpr inline uint16_t ntohs16(uint16_t v) { return byte_swap16(v); }  ///< network -> host

}  // namespace cinux::net
