/**
 * @file test/unit/test_numeric.cpp
 * @brief Tests for cinux::lib::Numeric utilities
 */

#include <catch2/catch_test_macros.hpp>
#include <cinux/numeric.hpp>

using namespace cinux::lib;
using namespace cinux::lib::literals;

// ============================================================
// Compile-time verification via static_assert
// ============================================================

static_assert(align_up(0, 4096) == 0);
static_assert(align_up(1, 4096) == 4096);
static_assert(align_up(4096, 4096) == 4096);
static_assert(align_up(4097, 4096) == 8192);

static_assert(align_down(0, 4096) == 0);
static_assert(align_down(4095, 4096) == 0);
static_assert(align_down(4096, 4096) == 4096);
static_assert(align_down(4097, 4096) == 4096);

static_assert(is_aligned(0, 4096));
static_assert(is_aligned(4096, 4096));
static_assert(!is_aligned(1, 4096));

static_assert(!is_power_of_two(0));
static_assert(is_power_of_two(1));
static_assert(is_power_of_two(256));
static_assert(is_power_of_two(4096));
static_assert(!is_power_of_two(3));
static_assert(!is_power_of_two(255));

static_assert(round_up_to_power_of_two(0) == 1);
static_assert(round_up_to_power_of_two(1) == 1);
static_assert(round_up_to_power_of_two(3) == 4);
static_assert(round_up_to_power_of_two(5) == 8);
static_assert(round_up_to_power_of_two(1024) == 1024);
static_assert(round_up_to_power_of_two(1025) == 2048);

static_assert(div_ceil(5, 4) == 2);
static_assert(div_ceil(4, 4) == 1);
static_assert(div_ceil(0, 4) == 0);
static_assert(div_ceil(1, 4) == 1);

static_assert(log2_int(1) == 0);
static_assert(log2_int(2) == 1);
static_assert(log2_int(4096) == 12);
static_assert(log2_int(0) == -1);

static_assert(4_KB == 4096ULL);
static_assert(1_MB == 1048576ULL);
static_assert(1_GB == 1073741824ULL);

// ============================================================
// Runtime tests
// ============================================================

TEST_CASE("numeric: align_up", "[numeric]") {
    REQUIRE(align_up(0, 4096) == 0);
    REQUIRE(align_up(1, 4096) == 4096);
    REQUIRE(align_up(4095, 4096) == 4096);
    REQUIRE(align_up(4096, 4096) == 4096);
    REQUIRE(align_up(4097, 4096) == 8192);

    // Non-power-of-two alignment
    REQUIRE(align_up(0, 3) == 0);
    REQUIRE(align_up(1, 3) == 3);
    REQUIRE(align_up(4, 3) == 6);
}

TEST_CASE("numeric: align_down", "[numeric]") {
    REQUIRE(align_down(4095, 4096) == 0);
    REQUIRE(align_down(4096, 4096) == 4096);
    REQUIRE(align_down(4097, 4096) == 4096);
    REQUIRE(align_down(8191, 4096) == 4096);
}

TEST_CASE("numeric: is_aligned", "[numeric]") {
    REQUIRE(is_aligned(0, 4096));
    REQUIRE(is_aligned(4096, 4096));
    REQUIRE(is_aligned(8192, 4096));
    REQUIRE_FALSE(is_aligned(1, 4096));
    REQUIRE_FALSE(is_aligned(100, 4096));
}

TEST_CASE("numeric: is_power_of_two", "[numeric]") {
    REQUIRE_FALSE(is_power_of_two(0));
    REQUIRE(is_power_of_two(1));
    REQUIRE(is_power_of_two(2));
    REQUIRE(is_power_of_two(256));
    REQUIRE(is_power_of_two(4096));
    REQUIRE_FALSE(is_power_of_two(3));
    REQUIRE_FALSE(is_power_of_two(100));
}

TEST_CASE("numeric: round_up_to_power_of_two", "[numeric]") {
    REQUIRE(round_up_to_power_of_two(0) == 1);
    REQUIRE(round_up_to_power_of_two(1) == 1);
    REQUIRE(round_up_to_power_of_two(2) == 2);
    REQUIRE(round_up_to_power_of_two(3) == 4);
    REQUIRE(round_up_to_power_of_two(5) == 8);
    REQUIRE(round_up_to_power_of_two(1024) == 1024);
    REQUIRE(round_up_to_power_of_two(1025) == 2048);
}

TEST_CASE("numeric: div_ceil", "[numeric]") {
    REQUIRE(div_ceil(0, 4) == 0);
    REQUIRE(div_ceil(1, 4) == 1);
    REQUIRE(div_ceil(4, 4) == 1);
    REQUIRE(div_ceil(5, 4) == 2);
    REQUIRE(div_ceil(8, 4) == 2);
    REQUIRE(div_ceil(9, 4) == 3);
}

TEST_CASE("numeric: log2_int", "[numeric]") {
    REQUIRE(log2_int(0) == -1);
    REQUIRE(log2_int(1) == 0);
    REQUIRE(log2_int(2) == 1);
    REQUIRE(log2_int(3) == 1);
    REQUIRE(log2_int(4) == 2);
    REQUIRE(log2_int(4096) == 12);
    REQUIRE(log2_int(65536) == 16);
}

TEST_CASE("numeric: memory literals", "[numeric]") {
    REQUIRE(1_KB == 1024ULL);
    REQUIRE(4_KB == 4096ULL);
    REQUIRE(1_MB == 1048576ULL);
    REQUIRE(1_GB == 1073741824ULL);
    REQUIRE(1_TB == 1099511627776ULL);
}
