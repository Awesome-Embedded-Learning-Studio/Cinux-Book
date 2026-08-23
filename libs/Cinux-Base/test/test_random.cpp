/**
 * @file test/unit/test_random.cpp
 * @brief Tests for cinux::lib::StaticRandomSource
 */

#include <catch2/catch_test_macros.hpp>
#include <cinux/random.hpp>
#include <cstring>

using namespace cinux::lib;

TEST_CASE("Random: unseeded state", "[random]") {
    StaticRandomSource rng;
    REQUIRE_FALSE(rng.seeded());
}

TEST_CASE("Random: deterministic sequence", "[random]") {
    StaticRandomSource a, b;
    a.seed(12345);
    b.seed(12345);

    for (int i = 0; i < 10; ++i) {
        REQUIRE(a.next_u64() == b.next_u64());
    }
    REQUIRE(a.seeded());
}

TEST_CASE("Random: different seeds give different sequences", "[random]") {
    StaticRandomSource a, b;
    a.seed(1);
    b.seed(2);

    bool any_different = false;
    for (int i = 0; i < 10; ++i) {
        if (a.next_u64() != b.next_u64()) {
            any_different = true;
        }
    }
    REQUIRE(any_different);
}

TEST_CASE("Random: next_u32", "[random]") {
    StaticRandomSource rng;
    rng.seed(42);
    uint32_t v = rng.next_u32();
    // Just ensure it doesn't crash
    (void)v;
}

TEST_CASE("Random: next_bounded", "[random]") {
    StaticRandomSource rng;
    rng.seed(42);

    for (int i = 0; i < 100; ++i) {
        uint32_t v = rng.next_bounded(10);
        REQUIRE(v < 10);
    }

    REQUIRE(rng.next_bounded(1) == 0);
    REQUIRE(rng.next_bounded(0) == 0);
}

TEST_CASE("Random: fill", "[random]") {
    StaticRandomSource rng;
    rng.seed(42);

    uint8_t buf[32] = {};
    rng.fill(buf, sizeof(buf));

    // At least some non-zero bytes
    int nonzero = 0;
    for (auto b : buf) {
        if (b != 0) {
            ++nonzero;
        }
    }
    REQUIRE(nonzero > 0);

    // Same seed gives same fill
    StaticRandomSource rng2;
    rng2.seed(42);
    uint8_t buf2[32] = {};
    rng2.fill(buf2, sizeof(buf2));
    REQUIRE(memcmp(buf, buf2, 32) == 0);
}
