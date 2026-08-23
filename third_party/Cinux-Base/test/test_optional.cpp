/**
 * @file test/unit/test_optional.cpp
 * @brief Tests for cinux::lib::Optional<T>
 */

#include <catch2/catch_test_macros.hpp>
#include <cinux/optional.hpp>
#include <string>

using namespace cinux::lib;

TEST_CASE("Optional: default empty", "[optional]") {
    Optional<int> o;
    REQUIRE_FALSE(o.has_value());
    REQUIRE_FALSE(static_cast<bool>(o));
}

TEST_CASE("Optional: nullopt", "[optional]") {
    Optional<int> o = nullopt;
    REQUIRE_FALSE(o.has_value());
}

TEST_CASE("Optional: value construction", "[optional]") {
    Optional<int> o(42);
    REQUIRE(o.has_value());
    REQUIRE(o.value() == 42);
    REQUIRE(*o == 42);
}

TEST_CASE("Optional: value_or", "[optional]") {
    Optional<int> a(10);
    Optional<int> b;

    REQUIRE(a.value_or(99) == 10);
    REQUIRE(b.value_or(99) == 99);
}

TEST_CASE("Optional: copy semantics", "[optional]") {
    Optional<int> a(42);
    Optional<int> b = a;
    REQUIRE(b.has_value());
    REQUIRE(b.value() == 42);
}

TEST_CASE("Optional: move semantics", "[optional]") {
    Optional<int> a(42);
    Optional<int> b = std::move(a);
    REQUIRE(b.has_value());
    REQUIRE(b.value() == 42);
}

TEST_CASE("Optional: reset", "[optional]") {
    Optional<int> o(42);
    REQUIRE(o.has_value());
    o.reset();
    REQUIRE_FALSE(o.has_value());
}

TEST_CASE("Optional: nullopt assignment", "[optional]") {
    Optional<int> o(42);
    o = nullopt;
    REQUIRE_FALSE(o.has_value());
}

TEST_CASE("Optional: emplace", "[optional]") {
    Optional<int> o;
    o.emplace(77);
    REQUIRE(o.has_value());
    REQUIRE(o.value() == 77);

    // emplace again replaces
    o.emplace(88);
    REQUIRE(o.value() == 88);
}

TEST_CASE("Optional: with non-trivial type", "[optional]") {
    Optional<std::string> o(std::string("hello"));
    REQUIRE(o.has_value());
    REQUIRE(o.value() == "hello");

    Optional<std::string> b = std::move(o);
    REQUIRE(b.has_value());
    REQUIRE(b.value() == "hello");
}

TEST_CASE("Optional: operator->", "[optional]") {
    struct Point {
        int x, y;
    };
    Optional<Point> p(Point{3, 4});
    REQUIRE(p->x == 3);
    REQUIRE(p->y == 4);
}
