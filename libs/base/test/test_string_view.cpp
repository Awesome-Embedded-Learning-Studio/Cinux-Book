/**
 * @file test/unit/test_string_view.cpp
 * @brief Tests for cinux::lib::StringView
 */

#include <catch2/catch_test_macros.hpp>
#include <cinux/string_view.hpp>

using namespace cinux::lib;

TEST_CASE("StringView: empty default", "[string_view]") {
    StringView sv;
    REQUIRE(sv.empty());
    REQUIRE(sv.size() == 0);
    REQUIRE(sv.data() == nullptr);
}

TEST_CASE("StringView: from C-string", "[string_view]") {
    StringView sv("hello");
    REQUIRE(sv.size() == 5);
    REQUIRE_FALSE(sv.empty());
    REQUIRE(sv[0] == 'h');
    REQUIRE(sv[4] == 'o');
}

TEST_CASE("StringView: from pointer+length", "[string_view]") {
    const char* s = "hello world";
    StringView  sv(s, 5);
    REQUIRE(sv.size() == 5);
    REQUIRE(sv == StringView("hello"));
}

TEST_CASE("StringView: front/back", "[string_view]") {
    StringView sv("abc");
    REQUIRE(sv.front() == 'a');
    REQUIRE(sv.back() == 'c');
}

TEST_CASE("StringView: out-of-bounds returns null", "[string_view]") {
    StringView sv("ab");
    REQUIRE(sv[5] == '\0');
}

TEST_CASE("StringView: comparison operators", "[string_view]") {
    StringView a("abc");
    StringView b("abc");
    StringView c("abd");
    StringView d("ab");

    REQUIRE(a == b);
    REQUIRE_FALSE(a != b);
    REQUIRE(a != c);
    REQUIRE(a < c);
    REQUIRE(a <= b);
    REQUIRE(c > a);
    REQUIRE(a >= b);
    REQUIRE(d < a);
    REQUIRE(a > d);
}

TEST_CASE("StringView: starts_with / ends_with", "[string_view]") {
    StringView sv("hello world");

    REQUIRE(sv.starts_with("hello"));
    REQUIRE(sv.starts_with(""));
    REQUIRE_FALSE(sv.starts_with("world"));

    REQUIRE(sv.ends_with("world"));
    REQUIRE(sv.ends_with(""));
    REQUIRE_FALSE(sv.ends_with("hello"));
}

TEST_CASE("StringView: find char", "[string_view]") {
    StringView sv("hello world");
    REQUIRE(sv.find('o') == 4);
    REQUIRE(sv.find('o', 5) == 7);
    REQUIRE(sv.find('z') == StringView::npos);
}

TEST_CASE("StringView: find substring", "[string_view]") {
    StringView sv("hello world");
    REQUIRE(sv.find("world") == 6);
    REQUIRE(sv.find("hello") == 0);
    REQUIRE(sv.find("xyz") == StringView::npos);
    REQUIRE(sv.find("") == 0);
}

TEST_CASE("StringView: rfind char", "[string_view]") {
    StringView sv("hello");
    REQUIRE(sv.rfind('l') == 3);
    REQUIRE(sv.rfind('z') == StringView::npos);
}

TEST_CASE("StringView: substr", "[string_view]") {
    StringView sv("hello world");

    REQUIRE(sv.substr(6) == StringView("world"));
    REQUIRE(sv.substr(0, 5) == StringView("hello"));
    REQUIRE(sv.substr(100) == StringView());
    REQUIRE(sv.substr(6, 100) == StringView("world"));
}
