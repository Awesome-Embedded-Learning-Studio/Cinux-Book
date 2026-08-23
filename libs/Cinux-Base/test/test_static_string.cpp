/**
 * @file test/unit/test_static_string.cpp
 * @brief Tests for cinux::lib::StaticString<N>
 */

#include <catch2/catch_test_macros.hpp>
#include <cinux/static_string.hpp>

using namespace cinux::lib;

TEST_CASE("StaticString: default empty", "[static_string]") {
    StaticString<16> s;
    REQUIRE(s.empty());
    REQUIRE(s.size() == 0);
    REQUIRE(s.c_str()[0] == '\0');
}

TEST_CASE("StaticString: from C-string", "[static_string]") {
    StaticString<32> s("hello");
    REQUIRE(s.size() == 5);
    REQUIRE(s == StringView("hello"));
}

TEST_CASE("StaticString: from StringView", "[static_string]") {
    StringView       sv("world");
    StaticString<16> s(sv);
    REQUIRE(s == sv);
}

TEST_CASE("StaticString: append char", "[static_string]") {
    StaticString<8> s;
    REQUIRE(s.append('a'));
    REQUIRE(s.append('b'));
    REQUIRE(s.size() == 2);
    REQUIRE(s == StringView("ab"));
}

TEST_CASE("StaticString: append overflow returns false", "[static_string]") {
    StaticString<5> s;  // capacity = 4 chars + null
    REQUIRE(s.append("abc"));
    REQUIRE(s.append('d'));        // 4th char, still fits
    REQUIRE_FALSE(s.append('e'));  // 5th char, exceeds
}

TEST_CASE("StaticString: view conversion", "[static_string]") {
    StaticString<16> s("test");
    StringView       sv = s;
    REQUIRE(sv.size() == 4);
    REQUIRE(sv == StringView("test"));
}

TEST_CASE("StaticString: clear and truncate", "[static_string]") {
    StaticString<16> s("hello world");
    s.truncate(5);
    REQUIRE(s == StringView("hello"));

    s.clear();
    REQUIRE(s.empty());
}

TEST_CASE("StaticString: parent_path", "[static_string]") {
    StaticString<64> p("/foo/bar");
    REQUIRE(p.parent_path() == StringView("/foo"));

    StaticString<64> root("/");
    REQUIRE(root.parent_path() == StringView("/"));

    StaticString<64> plain("file");
    REQUIRE(plain.parent_path() == StringView(""));
}

TEST_CASE("StaticString: filename", "[static_string]") {
    StaticString<64> p("/foo/bar.txt");
    REQUIRE(p.filename() == StringView("bar.txt"));

    StaticString<64> root("/");
    REQUIRE(root.filename() == StringView(""));

    StaticString<64> no_slash("name");
    REQUIRE(no_slash.filename() == StringView("name"));
}

TEST_CASE("StaticString: PathString and NameString aliases", "[static_string]") {
    PathString path("/usr/local/bin");
    REQUIRE(path.size() == 14);

    NameString name("test.txt");
    REQUIRE(name.size() == 8);
}
