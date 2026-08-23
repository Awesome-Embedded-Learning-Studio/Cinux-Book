/**
 * @file test/unit/test_bitmap.cpp
 * @brief Tests for cinux::lib::BitMap<N>
 */

#include <catch2/catch_test_macros.hpp>
#include <cinux/bitmap.hpp>

using namespace cinux::lib;

TEST_CASE("BitMap: set/clear/test round-trip", "[bitmap]") {
    BitMap<64> bm;
    REQUIRE_FALSE(bm.test(5));
    bm.set(5);
    REQUIRE(bm.test(5));
    bm.clear(5);
    REQUIRE_FALSE(bm.test(5));
}

TEST_CASE("BitMap: toggle", "[bitmap]") {
    BitMap<32> bm;
    bm.toggle(3);
    REQUIRE(bm.test(3));
    bm.toggle(3);
    REQUIRE_FALSE(bm.test(3));
}

TEST_CASE("BitMap: set_all / clear_all", "[bitmap]") {
    BitMap<64> bm;
    bm.set_all();
    REQUIRE(bm.count_set() == 64);
    REQUIRE(bm.count_clear() == 0);

    bm.clear_all();
    REQUIRE(bm.count_set() == 0);
    REQUIRE(bm.count_clear() == 64);
}

TEST_CASE("BitMap: find_first_clear after partial set", "[bitmap]") {
    BitMap<64> bm;
    for (size_t i = 0; i < 10; ++i) {
        bm.set(i);
    }
    REQUIRE(bm.find_first_clear() == 10);
}

TEST_CASE("BitMap: find_first_set", "[bitmap]") {
    BitMap<64> bm;
    bm.set(42);
    REQUIRE(bm.find_first_set() == 42);
    bm.clear_all();
    REQUIRE(bm.find_first_set() == 64);  // N = not found
}

TEST_CASE("BitMap: set_range / clear_range", "[bitmap]") {
    BitMap<128> bm;
    bm.set_range(5, 15);
    REQUIRE(bm.count_set() == 10);
    for (size_t i = 5; i < 15; ++i) {
        REQUIRE(bm.test(i));
    }
    REQUIRE_FALSE(bm.test(4));
    REQUIRE_FALSE(bm.test(15));
}

TEST_CASE("BitMap: boundary N=1", "[bitmap]") {
    BitMap<1> bm;
    REQUIRE_FALSE(bm.test(0));
    bm.set(0);
    REQUIRE(bm.test(0));
    REQUIRE(bm.count_set() == 1);
}

TEST_CASE("BitMap: boundary N=65 (crosses word)", "[bitmap]") {
    BitMap<65> bm;
    bm.set(64);
    REQUIRE(bm.test(64));
    REQUIRE(bm.count_set() == 1);

    bm.set_all();
    REQUIRE(bm.count_set() == 65);
}
