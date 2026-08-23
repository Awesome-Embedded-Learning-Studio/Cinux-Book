/**
 * @file test/unit/test_expected.cpp
 * @brief Tests for cinux::lib::ErrorOr<T> and ErrorOr<void>
 */

#include <catch2/catch_test_macros.hpp>
#include <cinux/expected.hpp>
#include <string>

using namespace cinux::lib;

// ============================================================
// Error enum
// ============================================================

TEST_CASE("Error: error_string covers all values", "[expected]") {
    REQUIRE(error_string(Error::Ok) != nullptr);
    REQUIRE(error_string(Error::OutOfMemory) != nullptr);
    REQUIRE(error_string(Error::NotFound) != nullptr);
    REQUIRE(error_string(Error::IOError) != nullptr);

    // All values should return non-null strings
    for (uint32_t i = 1; i <= static_cast<uint32_t>(Error::Busy); ++i) {
        REQUIRE(error_string(static_cast<Error>(i)) != nullptr);
    }
}

// ============================================================
// ErrorOr<T> — success path
// ============================================================

TEST_CASE("ErrorOr<int>: success path", "[expected]") {
    ErrorOr<int> result(42);
    REQUIRE(result.ok());
    REQUIRE(static_cast<bool>(result));
    REQUIRE(result.value() == 42);
    REQUIRE(*result == 42);
}

TEST_CASE("ErrorOr<int>: error path", "[expected]") {
    ErrorOr<int> result(Error::NotFound);
    REQUIRE_FALSE(result.ok());
    REQUIRE_FALSE(static_cast<bool>(result));
    REQUIRE(result.error() == Error::NotFound);
}

TEST_CASE("ErrorOr<int>: copy and move", "[expected]") {
    ErrorOr<int> a(10);
    ErrorOr<int> b = a;
    REQUIRE(b.ok());
    REQUIRE(b.value() == 10);

    ErrorOr<int> c = ErrorOr<int>(20);
    REQUIRE(c.ok());
    REQUIRE(c.value() == 20);
}

// ============================================================
// ErrorOr<T> with non-trivial type
// ============================================================

TEST_CASE("ErrorOr<std::string>: success", "[expected]") {
    ErrorOr<std::string> result(std::string("hello"));
    REQUIRE(result.ok());
    REQUIRE(result.value() == "hello");
}

TEST_CASE("ErrorOr<std::string>: error", "[expected]") {
    ErrorOr<std::string> result(Error::IOError);
    REQUIRE_FALSE(result.ok());
    REQUIRE(result.error() == Error::IOError);
}

// ============================================================
// ErrorOr<void>
// ============================================================

TEST_CASE("ErrorOr<void>: success", "[expected]") {
    ErrorOr<void> result;
    REQUIRE(result.ok());
    REQUIRE(static_cast<bool>(result));
}

TEST_CASE("ErrorOr<void>: error", "[expected]") {
    ErrorOr<void> result(Error::TimedOut);
    REQUIRE_FALSE(result.ok());
    REQUIRE(result.error() == Error::TimedOut);
}

// ============================================================
// Operator-> test
// ============================================================

struct Point {
    int x, y;
};

TEST_CASE("ErrorOr: operator->", "[expected]") {
    ErrorOr<Point> p(Point{3, 4});
    REQUIRE(p.ok());
    REQUIRE(p->x == 3);
    REQUIRE(p->y == 4);
}
