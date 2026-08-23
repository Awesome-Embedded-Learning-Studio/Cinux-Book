/**
 * @file test/unit/test_smoke.cpp
 * @brief Smoke test — verifies the build infrastructure works
 */

#include <catch2/catch_test_macros.hpp>
#include <cstdint>

TEST_CASE("smoke: basic arithmetic", "[smoke]") {
    REQUIRE(1 + 1 == 2);
    REQUIRE(true);
    REQUIRE_FALSE(false);
}

TEST_CASE("smoke: integer comparisons", "[smoke]") {
    int a = 10;
    int b = 20;

    REQUIRE(a < b);
    REQUIRE(b > a);
    REQUIRE(a != b);
    REQUIRE(a <= 10);
    REQUIRE(b >= 20);
}

TEST_CASE("smoke: C++17 features", "[smoke]") {
    // Verify C++17 compilation is active
    constexpr auto val = [](int x) { return x * 2; }(21);
    REQUIRE(val == 42);

    // Verify constexpr if
    constexpr bool is_64bit = sizeof(void*) == 8;
    if constexpr (is_64bit) {
        REQUIRE(sizeof(std::uintptr_t) == 8);
    } else {
        REQUIRE(sizeof(std::uintptr_t) == 4);
    }
}
