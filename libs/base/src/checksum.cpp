/**
 * @file checksum.cpp
 * @brief Internet checksum implementation (RFC 1071).
 */

#include <cinux/checksum.hpp>
#include <cstddef>
#include <cstring>

namespace cinux::lib {

uint16_t internet_checksum(const void* data, size_t len) {
    return internet_checksum(ConstByteSpan(static_cast<const uint8_t*>(data), len));
}

uint16_t internet_checksum(ConstByteSpan data) {
    uint32_t sum = 0;

    // Accumulate 16-bit words
    size_t i = 0;
    for (; i + 1 < data.size(); i += 2) {
        uint16_t word = (static_cast<uint16_t>(data[i]) << 8) | static_cast<uint16_t>(data[i + 1]);
        sum += word;
    }

    // Handle odd trailing byte
    if (i < data.size()) {
        sum += static_cast<uint16_t>(data[i]) << 8;
    }

    return finalize_checksum(sum);
}

bool verify_internet_checksum(const void* data, size_t len) {
    uint16_t result = internet_checksum(data, len);
    // Valid if the result is 0x0000 (complement of 0xFFFF)
    return result == 0x0000;
}

uint32_t pseudo_header_partial(uint32_t src_ip, uint32_t dst_ip, uint8_t protocol,
                               uint16_t payload_len) {
    uint32_t sum = 0;

    // Source IP (two 16-bit words, network order)
    sum += (src_ip >> 16) & 0xFFFF;
    sum += src_ip & 0xFFFF;

    // Destination IP
    sum += (dst_ip >> 16) & 0xFFFF;
    sum += dst_ip & 0xFFFF;

    // Protocol (zero-extended to 16 bits)
    sum += static_cast<uint16_t>(protocol);

    // Payload length (TCP/UDP length field)
    sum += payload_len;

    return sum;
}

uint16_t finalize_checksum(uint32_t partial_sum) {
    // Fold 32-bit sum to 16 bits
    while (partial_sum >> 16) {
        partial_sum = (partial_sum & 0xFFFF) + (partial_sum >> 16);
    }
    // One's complement
    return static_cast<uint16_t>(~partial_sum);
}

}  // namespace cinux::lib
