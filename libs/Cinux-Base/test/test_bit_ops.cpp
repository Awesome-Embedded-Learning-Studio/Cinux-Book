/**
 * @file test/unit/test_bit_ops.cpp
 * @brief Tests for cinux::lib::BitOps
 */

#include <catch2/catch_test_macros.hpp>
#include <cinux/bit_ops.hpp>

using namespace cinux::lib;

// ============================================================
// Compile-time verification
// ============================================================

static_assert(popcount(uint32_t{0}) == 0);
static_assert(popcount(uint32_t{0xFF}) == 8);
static_assert(popcount(uint64_t{0}) == 0);
static_assert(popcount(uint64_t{0xFFFFFFFFFFFFFFFFULL}) == 64);

static_assert(ctz(uint32_t{8}) == 3);
static_assert(clz(uint32_t{1}) == 31);
static_assert(ctz(uint64_t{1}) == 0);
static_assert(clz(uint64_t{1}) == 63);

static_assert(set_bit(uint64_t{0}, 5) == 32);
static_assert(clear_bit(uint64_t{32}, 5) == 0);
static_assert(test_bit(uint64_t{32}, 5));
static_assert(!test_bit(uint64_t{32}, 0));

// ============================================================
// Runtime tests
// ============================================================

TEST_CASE("bit_ops: popcount", "[bit_ops]") {
    REQUIRE(popcount(uint32_t{0}) == 0);
    REQUIRE(popcount(uint32_t{0xFF}) == 8);
    REQUIRE(popcount(uint32_t{0xFFFF}) == 16);
    REQUIRE(popcount(uint32_t{0x1}) == 1);
    REQUIRE(popcount(uint64_t{0}) == 0);
    REQUIRE(popcount(uint64_t{0xFFFFFFFFFFFFFFFFULL}) == 64);
}

TEST_CASE("bit_ops: clz", "[bit_ops]") {
    REQUIRE(clz(uint32_t{1}) == 31);
    REQUIRE(clz(uint32_t{0x80000000u}) == 0);
    REQUIRE(clz(uint64_t{1}) == 63);
    REQUIRE(clz(uint64_t{1ULL << 63}) == 0);
    REQUIRE(clz(uint32_t{0}) == 32);
}

TEST_CASE("bit_ops: ctz", "[bit_ops]") {
    REQUIRE(ctz(uint32_t{1}) == 0);
    REQUIRE(ctz(uint32_t{8}) == 3);
    REQUIRE(ctz(uint64_t{1}) == 0);
    REQUIRE(ctz(uint64_t{256}) == 8);
    REQUIRE(ctz(uint32_t{0}) == 32);
}

TEST_CASE("bit_ops: rotl/rotr", "[bit_ops]") {
    uint64_t v = 0x0123456789ABCDEFULL;
    // rotl then rotr should round-trip
    REQUIRE(rotr(rotl(v, 13), 13) == v);
    REQUIRE(rotl(rotr(v, 37), 37) == v);
    // rotl by 0 is identity
    REQUIRE(rotl(v, 0) == v);
    REQUIRE(rotr(v, 0) == v);
    // rotl by 64 is identity
    REQUIRE(rotl(v, 64) == v);
}

TEST_CASE("bit_ops: single-bit operations", "[bit_ops]") {
    REQUIRE(bit(0) == 1);
    REQUIRE(bit(5) == 32);
    REQUIRE(bit(63) == (1ULL << 63));

    REQUIRE(set_bit(0, 5) == 32);
    REQUIRE(clear_bit(32, 5) == 0);
    REQUIRE(toggle_bit(0, 3) == 8);
    REQUIRE(toggle_bit(8, 3) == 0);
    REQUIRE(test_bit(32, 5));
    REQUIRE_FALSE(test_bit(32, 0));
}

TEST_CASE("bit_ops: extract_bits / insert_bits", "[bit_ops]") {
    // 0xABCD = 1010_1011_1100_1101
    // Bits [11:8] = 1011 = 0xB
    REQUIRE(extract_bits(0xABCD, 11, 8) == 0xB);

    // Bits [7:4] = 1100 = 0xC
    REQUIRE(extract_bits(0xABCD, 7, 4) == 0xC);

    // Insert 0x5 into bits [11:8] of 0xABCD
    // Result: 1010_0101_1100_1101 = 0xA5CD
    REQUIRE(insert_bits(0xABCD, 11, 8, 0x5) == 0xA5CD);

    // Extract full width
    REQUIRE(extract_bits(0xFF, 7, 0) == 0xFF);

    // Edge: bit [63:63]
    uint64_t v = 1ULL << 63;
    REQUIRE(extract_bits(v, 63, 63) == 1);
    REQUIRE(insert_bits(0ULL, 63, 63, 1) == v);
}
