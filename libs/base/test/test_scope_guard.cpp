/**
 * @file test/unit/test_scope_guard.cpp
 * @brief Tests for cinux::lib::ScopeGuard and SCOPE_EXIT macro
 */

#include <catch2/catch_test_macros.hpp>
#include <cinux/scope_guard.hpp>

using namespace cinux::lib;

TEST_CASE("ScopeGuard: fires on scope exit", "[scope_guard]") {
    int counter = 0;
    {
        ScopeGuard guard([&] { counter++; });
        REQUIRE(counter == 0);
    }
    REQUIRE(counter == 1);
}

TEST_CASE("ScopeGuard: dismiss prevents execution", "[scope_guard]") {
    int counter = 0;
    {
        ScopeGuard guard([&] { counter++; });
        guard.dismiss();
    }
    REQUIRE(counter == 0);
}

TEST_CASE("ScopeGuard: move transfers ownership", "[scope_guard]") {
    int counter = 0;
    {
        ScopeGuard first([&] { counter++; });
        ScopeGuard second(std::move(first));
        // first is now inactive — should not fire
    }
    REQUIRE(counter == 1);
}

TEST_CASE("ScopeGuard: SCOPE_EXIT macro", "[scope_guard]") {
    int counter = 0;
    {
        SCOPE_EXIT(counter = 42);
        REQUIRE(counter == 0);
    }
    REQUIRE(counter == 42);
}

TEST_CASE("ScopeGuard: multiple guards fire in reverse order", "[scope_guard]") {
    int trace = 0;
    {
        ScopeGuard g1([&] { trace = trace * 10 + 1; });
        ScopeGuard g2([&] { trace = trace * 10 + 2; });
        ScopeGuard g3([&] { trace = trace * 10 + 3; });
    }
    // Destruction order: g3 -> g2 -> g1
    REQUIRE(trace == 321);
}
