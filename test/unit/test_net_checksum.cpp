/**
 * @file test/unit/test_net_checksum.cpp
 * @brief Host unit tests for cinux::lib::internet_checksum (RFC 1071).
 *
 * The internet checksum is the #1 "green tests, ping silently fails" trap
 * (SLIRP drops bad-checksum ICMP).  Hand-verified known vectors anchor the
 * implementation; an IPv4-header round-trip proves verify_internet_checksum.
 *
 * Links third_party/Cinux-Base/src/checksum.cpp (the real implementation -- not
 * a re-derivation).
 *
 * Compile condition: -DCINUX_HOST_TEST
 */

#define TEST_FRAMEWORK_IMPL

#include <cinux/checksum.hpp>
#include <cstdint>
#include <cstring>

#include "test_framework.h"

using cinux::lib::internet_checksum;
using cinux::lib::verify_internet_checksum;

// ============================================================
// Hand-verified known vectors (ones-complement sum, big-endian words)
// ============================================================

TEST("checksum: single word 0x1234 -> ~0x1234 = 0xEDCB") {
    const uint8_t d[] = {0x12, 0x34};
    ASSERT_EQ(internet_checksum(d, 2), 0xEDCBu);
}

TEST("checksum: all-zero payload -> ~0 = 0xFFFF") {
    const uint8_t d[] = {0x00, 0x00, 0x00, 0x00};
    ASSERT_EQ(internet_checksum(d, 4), 0xFFFFu);
}

TEST("checksum: odd length pads the trailing byte with a zero low byte") {
    // 0x1234 + 0x5600 = 0x6834; ~0x6834 = 0x97CB
    const uint8_t d[] = {0x12, 0x34, 0x56};
    ASSERT_EQ(internet_checksum(d, 3), 0x97CBu);
}

TEST("checksum: carry fold (0xFFFF+0xFFFF -> 0x0000)") {
    // 0xFFFF+0xFFFF = 0x1FFFE -> fold 0xFFFE+1 = 0xFFFF -> ~ = 0x0000
    const uint8_t d[] = {0xFF, 0xFF, 0xFF, 0xFF};
    ASSERT_EQ(internet_checksum(d, 4), 0x0000u);
}

// ============================================================
// IPv4 header round-trip: compute -> embed -> verify
// ============================================================

namespace {

/// 20-byte IPv4 header with checksum field zeroed (ready to compute over).
void build_ipv4_header(uint8_t* p) {
    // ver/IHL=0x45, DSCP=0, total_len=20 (BE), id=1, flags/frag=0, TTL=64,
    // proto=1 (ICMP), checksum=0 (filled by caller), src=10.0.2.15, dst=10.0.2.2.
    static const uint8_t tpl[20] = {0x45, 0x00, 0x00, 0x14, 0x00, 0x01, 0x00, 0x00, 0x40, 0x01,
                                    0x00, 0x00, 0x0A, 0x00, 0x02, 0x0F, 0x0A, 0x00, 0x02, 0x02};
    std::memcpy(p, tpl, 20);
}

}  // namespace

TEST("checksum: IPv4 header round-trip verifies after embedding") {
    uint8_t hdr[20];
    build_ipv4_header(hdr);
    // Compute over the header with the checksum field zero, embed big-endian.
    const uint16_t cs = internet_checksum(hdr, 20);
    hdr[10]           = static_cast<uint8_t>(cs >> 8);
    hdr[11]           = static_cast<uint8_t>(cs & 0xFF);
    // Re-summing a valid-checksum header yields 0xFFFF -> verify returns true.
    ASSERT_TRUE(verify_internet_checksum(hdr, 20));
}

TEST("checksum: a corrupted header fails verification") {
    uint8_t hdr[20];
    build_ipv4_header(hdr);
    const uint16_t cs = internet_checksum(hdr, 20);
    hdr[10]           = static_cast<uint8_t>(cs >> 8);
    hdr[11]           = static_cast<uint8_t>(cs & 0xFF);
    hdr[8] ^= 0xFF;  // corrupt TTL
    ASSERT_FALSE(verify_internet_checksum(hdr, 20));
}

int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}
