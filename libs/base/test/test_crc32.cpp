/**
 * @file test/unit/test_crc32.cpp
 * @brief Tests for cinux::lib::crc32
 */

#include <catch2/catch_test_macros.hpp>
#include <cinux/crc32.hpp>

using namespace cinux::lib;

TEST_CASE("crc32: known answer '123456789'", "[crc32]") {
    // The standard CRC32 check value
    const char* data = "123456789";
    REQUIRE(crc32(data, 9) == 0xCBF43926u);
}

TEST_CASE("crc32: empty input returns 0", "[crc32]") {
    REQUIRE(crc32(nullptr, 0) == 0x00000000u);
}

TEST_CASE("crc32: single byte", "[crc32]") {
    // CRC32 of a zero byte
    uint8_t  zero   = 0;
    uint32_t result = crc32(&zero, 1);
    REQUIRE(result != 0);  // Should be a non-zero checksum
}

TEST_CASE("crc32: deterministic", "[crc32]") {
    const char* msg    = "Hello, Cinux!";
    uint32_t    first  = crc32(msg, 13);
    uint32_t    second = crc32(msg, 13);
    REQUIRE(first == second);
}

TEST_CASE("crc32: different inputs give different checksums", "[crc32]") {
    const char* a = "foo";
    const char* b = "bar";
    REQUIRE(crc32(a, 3) != crc32(b, 3));
}
