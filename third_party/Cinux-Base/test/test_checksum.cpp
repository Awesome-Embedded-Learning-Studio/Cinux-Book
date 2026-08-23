/**
 * @file test/unit/test_checksum.cpp
 * @brief Tests for cinux::lib::internet_checksum
 */

#include <catch2/catch_test_macros.hpp>
#include <cinux/checksum.hpp>

using namespace cinux::lib;

TEST_CASE("checksum: zero-length returns 0xFFFF", "[checksum]") {
    // Sum of zero words = 0, complement = 0xFFFF
    uint8_t  dummy = 0;
    uint16_t cs    = internet_checksum(&dummy, 0);
    REQUIRE(cs == 0xFFFF);
}

TEST_CASE("checksum: finalize folds carry", "[checksum]") {
    // 0x1FFFF -> fold: 0xFFFF + 0x1 = 0x10000 -> fold again: 0x0 + 0x1 = 0x1 -> ~0x1 = 0xFFFE
    REQUIRE(finalize_checksum(0x1FFFF) == 0xFFFE);
}

TEST_CASE("checksum: known IP header", "[checksum]") {
    // Minimal IP header (20 bytes) with checksum field zeroed
    // Version=4, IHL=5, TOS=0, Total=20, ID=0, Flags=0, TTL=64, Proto=6(TCP), Sum=0, Src=127.0.0.1,
    // Dst=127.0.0.1
    uint8_t header[] = {
        0x45, 0x00, 0x00, 0x14, 0x00, 0x00,
        0x00, 0x00, 0x40, 0x06, 0x00, 0x00,  // checksum = 0 (to be computed)
        0x7F, 0x00, 0x00, 0x01,              // src = 127.0.0.1
        0x7F, 0x00, 0x00, 0x01               // dst = 127.0.0.1
    };

    uint16_t cs = internet_checksum(header, 20);
    // The checksum should be non-zero for a valid header
    REQUIRE(cs != 0x0000);

    // Set the checksum field and verify
    header[10] = static_cast<uint8_t>(cs >> 8);
    header[11] = static_cast<uint8_t>(cs & 0xFF);
    REQUIRE(verify_internet_checksum(header, 20));
}

TEST_CASE("checksum: pseudo_header_partial", "[checksum]") {
    uint32_t partial = pseudo_header_partial(0x7F000001, 0x7F000001, 6, 20);
    REQUIRE(partial != 0);

    // src + dst = (0x7F00 + 0x0001) * 2 = 0x7F00*2 + 0x0001*2 = 0xFE00 + 0x0002 = 0xFE02
    // proto = 6, len = 20
    // total = 0xFE02 + 6 + 20
    REQUIRE(partial == 0xFE02 + 6 + 20);
}
