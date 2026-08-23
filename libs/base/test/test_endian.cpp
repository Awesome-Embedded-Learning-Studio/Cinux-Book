/**
 * @file test/test_endian.cpp
 * @brief Tests for cinux::lib::Endian conversion utilities
 */

#include <catch2/catch_test_macros.hpp>
#include <cinux/endian.hpp>

// NOTE: Do NOT use "using namespace cinux::lib" here —
// names like htons/ntohs/bswap16 clash with GCC builtins in <endian.h>.

// ============================================================
// Compile-time verification
// ============================================================

static_assert(cinux::lib::bswap16(uint16_t{0x1234}) == 0x3412);
static_assert(cinux::lib::bswap32(uint32_t{0x12345678}) == 0x78563412);

// ============================================================
// Runtime tests
// ============================================================

TEST_CASE("endian: bswap16", "[endian]") {
    using cinux::lib::bswap16;
    REQUIRE(bswap16(uint16_t{0x1234}) == 0x3412);
    REQUIRE(bswap16(uint16_t{0x0001}) == 0x0100);
    REQUIRE(bswap16(uint16_t{0xFF00}) == 0x00FF);
    REQUIRE(bswap16(uint16_t{0x0000}) == 0x0000);
}

TEST_CASE("endian: bswap32", "[endian]") {
    using cinux::lib::bswap32;
    REQUIRE(bswap32(uint32_t{0x12345678}) == 0x78563412);
    REQUIRE(bswap32(uint32_t{0x00000001}) == 0x01000000);
    REQUIRE(bswap32(uint32_t{0xFF000000}) == 0x000000FF);
}

TEST_CASE("endian: bswap64", "[endian]") {
    using cinux::lib::bswap64;
    REQUIRE(bswap64(uint64_t{0x0123456789ABCDEFULL}) == 0xEFCDAB8967452301ULL);
}

TEST_CASE("endian: host-endian detection", "[endian]") {
    REQUIRE(cinux::lib::is_little_endian());
    REQUIRE_FALSE(cinux::lib::is_big_endian());
}

TEST_CASE("endian: htobe / betoh round-trip", "[endian]") {
    using cinux::lib::betoh16;
    using cinux::lib::betoh32;
    using cinux::lib::betoh64;
    using cinux::lib::htobe16;
    using cinux::lib::htobe32;
    using cinux::lib::htobe64;
    REQUIRE(betoh16(htobe16(uint16_t{0xABCD})) == 0xABCD);
    REQUIRE(betoh32(htobe32(uint32_t{0xDEADBEEFu})) == 0xDEADBEEFu);
    REQUIRE(betoh64(htobe64(uint64_t{0x0123456789ABCDEFULL})) == 0x0123456789ABCDEFULL);
}

TEST_CASE("endian: htole / letoh round-trip", "[endian]") {
    using cinux::lib::htole16;
    using cinux::lib::htole32;
    using cinux::lib::htole64;
    using cinux::lib::letoh16;
    using cinux::lib::letoh32;
    using cinux::lib::letoh64;
    REQUIRE(letoh16(htole16(uint16_t{0xABCD})) == 0xABCD);
    REQUIRE(letoh32(htole32(uint32_t{0xDEADBEEFu})) == 0xDEADBEEFu);
    REQUIRE(letoh64(htole64(uint64_t{0x0123456789ABCDEFULL})) == 0x0123456789ABCDEFULL);
}

TEST_CASE("endian: htobe16 on little-endian host", "[endian]") {
    using cinux::lib::htobe16;
    using cinux::lib::htole16;
    if constexpr (cinux::lib::is_little_endian()) {
        REQUIRE(htobe16(uint16_t{0x0001}) == 0x0100);
        REQUIRE(htole16(uint16_t{0x0001}) == 0x0001);
    }
}

TEST_CASE("endian: POSIX aliases", "[endian]") {
    using cinux::lib::htonl;
    using cinux::lib::htons;
    using cinux::lib::ntohl;
    using cinux::lib::ntohs;
    REQUIRE(ntohs(htons(uint16_t{0x1234})) == 0x1234);
    REQUIRE(ntohl(htonl(uint32_t{0x12345678})) == 0x12345678);
}
