/**
 * @file test/unit/test_vformat.cpp
 * @brief Tests for cinux::lib::detail::vformat
 */

#include <catch2/catch_test_macros.hpp>
#include <cinux/detail/vformat.hpp>
#include <cstdarg>
#include <cstring>

using namespace cinux::lib::detail;

/** @brief Helper: format into a std::string-like buffer for testing. */
static int format_to_buf(char* buf, size_t size, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vformat_to_buf(buf, size, fmt, args);
    va_end(args);
    return static_cast<int>(strlen(buf));
}

TEST_CASE("vformat: literal text", "[vformat]") {
    char buf[64];
    format_to_buf(buf, sizeof(buf), "hello");
    REQUIRE(strcmp(buf, "hello") == 0);
}

TEST_CASE("vformat: %% escape", "[vformat]") {
    char buf[64];
    format_to_buf(buf, sizeof(buf), "100%%");
    REQUIRE(strcmp(buf, "100%") == 0);
}

TEST_CASE("vformat: %s", "[vformat]") {
    char buf[64];
    format_to_buf(buf, sizeof(buf), "hello %s", "world");
    REQUIRE(strcmp(buf, "hello world") == 0);
}

TEST_CASE("vformat: %d positive", "[vformat]") {
    char buf[64];
    format_to_buf(buf, sizeof(buf), "num=%d", 42);
    REQUIRE(strcmp(buf, "num=42") == 0);
}

TEST_CASE("vformat: %d negative", "[vformat]") {
    char buf[64];
    format_to_buf(buf, sizeof(buf), "%d", -7);
    REQUIRE(strcmp(buf, "-7") == 0);
}

TEST_CASE("vformat: %u", "[vformat]") {
    char buf[64];
    format_to_buf(buf, sizeof(buf), "%u", 12345u);
    REQUIRE(strcmp(buf, "12345") == 0);
}

TEST_CASE("vformat: %x", "[vformat]") {
    char buf[64];
    format_to_buf(buf, sizeof(buf), "%x", 0xFF);
    REQUIRE(strcmp(buf, "ff") == 0);
}

TEST_CASE("vformat: %X", "[vformat]") {
    char buf[64];
    format_to_buf(buf, sizeof(buf), "%X", 0xAB);
    REQUIRE(strcmp(buf, "AB") == 0);
}

TEST_CASE("vformat: %c", "[vformat]") {
    char buf[64];
    format_to_buf(buf, sizeof(buf), "%c", 'Z');
    REQUIRE(strcmp(buf, "Z") == 0);
}

TEST_CASE("vformat: %p", "[vformat]") {
    char buf[64];
    format_to_buf(buf, sizeof(buf), "%p", static_cast<uint64_t>(0x1234));
    REQUIRE(buf[0] == '0');
    REQUIRE(buf[1] == 'x');
    // Should be zero-padded to 16 digits
    REQUIRE(strlen(buf) == 18);  // "0x" + 16 digits
}

TEST_CASE("vformat: width and zero-pad", "[vformat]") {
    char buf[64];
    format_to_buf(buf, sizeof(buf), "%05d", 42);
    REQUIRE(strcmp(buf, "00042") == 0);
}

TEST_CASE("vformat: left-align", "[vformat]") {
    char buf[64];
    format_to_buf(buf, sizeof(buf), "%-5d|", 42);
    REQUIRE(strcmp(buf, "42   |") == 0);
}

TEST_CASE("vformat: combined specifiers", "[vformat]") {
    char buf[128];
    format_to_buf(buf, sizeof(buf), "%s %d %x", "hello", 42, 0xFF);
    REQUIRE(strcmp(buf, "hello 42 ff") == 0);
}

TEST_CASE("vformat: buffer overflow truncation", "[vformat]") {
    char buf[5];
    format_to_buf(buf, sizeof(buf), "abcdefgh");
    REQUIRE(strlen(buf) == 4);  // truncated + null
}
