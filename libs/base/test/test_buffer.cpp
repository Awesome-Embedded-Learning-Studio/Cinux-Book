/**
 * @file test/unit/test_buffer.cpp
 * @brief Tests for cinux::lib::BufferView and StaticBuffer<N>
 */

#include <catch2/catch_test_macros.hpp>
#include <cinux/buffer.hpp>

using namespace cinux::lib;

TEST_CASE("BufferView: default empty", "[buffer]") {
    BufferView bv;
    REQUIRE(bv.empty());
    REQUIRE(bv.size() == 0);
}

TEST_CASE("BufferView: slice truncates", "[buffer]") {
    uint8_t    data[] = {0x01, 0x02, 0x03, 0x04, 0x05};
    BufferView bv(data, 5);

    auto s = bv.slice(2, 2);
    REQUIRE(s.size() == 2);
    REQUIRE(s[0] == 0x03);
    REQUIRE(s[1] == 0x04);

    // Out of bounds
    auto s2 = bv.slice(10, 2);
    REQUIRE(s2.empty());

    // Partial
    auto s3 = bv.slice(3, 10);
    REQUIRE(s3.size() == 2);
}

TEST_CASE("BufferView: as_string", "[buffer]") {
    const char* msg = "hello";
    BufferView  bv(msg, 5);
    StringView  sv = bv.as_string();
    REQUIRE(sv == StringView("hello"));
}

TEST_CASE("StaticBuffer: copy_from / copy_to", "[buffer]") {
    StaticBuffer<16> buf;
    const char*      src = "ABCDE";
    buf.copy_from(src, 5);
    REQUIRE(buf.size() == 5);

    char dst[16] = {};
    buf.copy_to(dst, 5);
    REQUIRE(dst[0] == 'A');
    REQUIRE(dst[4] == 'E');
}

TEST_CASE("StaticBuffer: view and as_span", "[buffer]") {
    StaticBuffer<8> buf;
    uint8_t         data[] = {0xAA, 0xBB, 0xCC};
    buf.copy_from(data, 3);

    auto v = buf.view();
    REQUIRE(v.size() == 3);
    REQUIRE(v[0] == 0xAA);

    auto span = buf.as_span();
    REQUIRE(span.size() == 3);
    REQUIRE(span[2] == 0xCC);
}

TEST_CASE("StaticBuffer: fill", "[buffer]") {
    StaticBuffer<4> buf;
    buf.fill(0xFF);
    REQUIRE(buf.size() == 4);
    for (size_t i = 0; i < 4; ++i) {
        REQUIRE(buf.data()[i] == 0xFF);
    }
}
