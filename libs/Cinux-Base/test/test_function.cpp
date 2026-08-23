/**
 * @file test/unit/test_function.cpp
 * @brief Tests for cinux::lib::Function<Sig, N>
 */

#include <catch2/catch_test_macros.hpp>
#include <cinux/function.hpp>

using namespace cinux::lib;

// ============================================================
// Free function for testing
// ============================================================

static int double_it(int x) {
    return x * 2;
}

// ============================================================
// Tests
// ============================================================

TEST_CASE("Function: default is invalid", "[function]") {
    Function<int(int)> f;
    REQUIRE_FALSE(f.valid());
    REQUIRE_FALSE(static_cast<bool>(f));
}

TEST_CASE("Function: nullptr", "[function]") {
    Function<int(int)> f = nullptr;
    REQUIRE_FALSE(f.valid());
}

TEST_CASE("Function: function pointer", "[function]") {
    Function<int(int)> f = double_it;
    REQUIRE(f.valid());
    REQUIRE(f(21) == 42);
}

TEST_CASE("Function: stateless lambda", "[function]") {
    Function<int(int)> f = [](int x) { return x + 1; };
    REQUIRE(f(9) == 10);
}

TEST_CASE("Function: stateful lambda (captures pointer)", "[function]") {
    int             value = 100;
    int*            ptr   = &value;
    Function<int()> f     = [ptr]() { return *ptr; };
    REQUIRE(f() == 100);
}

TEST_CASE("Function: copy", "[function]") {
    Function<int(int)> a = [](int x) { return x * 3; };
    Function<int(int)> b = a;
    REQUIRE(b.valid());
    REQUIRE(b(5) == 15);
    REQUIRE(a.valid());  // original still valid
}

TEST_CASE("Function: move", "[function]") {
    Function<int(int)> a = [](int x) { return x * 3; };
    Function<int(int)> b = std::move(a);
    REQUIRE(b.valid());
    REQUIRE(b(5) == 15);
    REQUIRE_FALSE(a.valid());  // moved-from is invalid
}

TEST_CASE("Function: reset", "[function]") {
    Function<int(int)> f = [](int x) { return x; };
    REQUIRE(f.valid());
    f.reset();
    REQUIRE_FALSE(f.valid());
}

TEST_CASE("Function: void return type", "[function]") {
    int              counter = 0;
    Function<void()> f       = [&counter]() { counter++; };
    f();
    REQUIRE(counter == 1);
}
