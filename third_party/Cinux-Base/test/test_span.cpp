/**
 * @file test/unit/test_span.cpp
 * @brief Tests for cinux::lib::Span<T>
 */

#include <catch2/catch_test_macros.hpp>
#include <cinux/span.hpp>

using namespace cinux::lib;

TEST_CASE("Span: default empty", "[span]") {
    Span<int> s;
    REQUIRE(s.empty());
    REQUIRE(s.size() == 0);
    REQUIRE(s.data() == nullptr);
}

TEST_CASE("Span: from pointer+length", "[span]") {
    int       arr[] = {10, 20, 30};
    Span<int> s(arr, 3);
    REQUIRE(s.size() == 3);
    REQUIRE(s[0] == 10);
    REQUIRE(s[2] == 30);
}

TEST_CASE("Span: from pointer pair", "[span]") {
    int       arr[] = {1, 2, 3, 4};
    Span<int> s(arr + 1, arr + 3);
    REQUIRE(s.size() == 2);
    REQUIRE(s[0] == 2);
    REQUIRE(s[1] == 3);
}

TEST_CASE("Span: from C array", "[span]") {
    int       arr[] = {5, 6, 7};
    Span<int> s(arr);
    REQUIRE(s.size() == 3);
    REQUIRE(s.front() == 5);
    REQUIRE(s.back() == 7);
}

TEST_CASE("Span: first/last/subspan", "[span]") {
    int       arr[] = {1, 2, 3, 4, 5};
    Span<int> s(arr);

    auto f = s.first(3);
    REQUIRE(f.size() == 3);
    REQUIRE(f[0] == 1);
    REQUIRE(f[2] == 3);

    auto l = s.last(2);
    REQUIRE(l.size() == 2);
    REQUIRE(l[0] == 4);
    REQUIRE(l[1] == 5);

    auto sub = s.subspan(1, 3);
    REQUIRE(sub.size() == 3);
    REQUIRE(sub[0] == 2);
    REQUIRE(sub[2] == 4);
}

TEST_CASE("Span: range-for iteration", "[span]") {
    int       arr[] = {10, 20, 30};
    Span<int> s(arr);
    int       sum = 0;
    for (auto& v : s) {
        sum += v;
    }
    REQUIRE(sum == 60);
}

TEST_CASE("Span: const T is read-only", "[span]") {
    const int       arr[] = {1, 2, 3};
    Span<const int> s(arr);
    REQUIRE(s.size() == 3);
    REQUIRE(s[0] == 1);
}

TEST_CASE("Span: ByteSpan aliases", "[span]") {
    uint8_t  buf[] = {0x01, 0x02, 0x03};
    ByteSpan bs(buf);
    REQUIRE(bs.size() == 3);
    REQUIRE(bs[1] == 0x02);

    const uint8_t cbuf[] = {0xAA, 0xBB};
    ConstByteSpan cbs(cbuf);
    REQUIRE(cbs.size() == 2);
}
