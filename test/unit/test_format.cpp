/**
 * @file    test_format.cpp
 * @brief   Behaviour tests for the base format engine.
 *
 * @author  Charliechen114514
 * @date    2026-09-25
 * @version 0.1
 * @since   0.1.0
 * @ingroup base_format
 */

#include <cinux/format.hpp>
#include <cstddef>
#include <cstdint>

#include "../framework/framework.hpp"

using cinux::base::format::VformatToBuf;
using cinux::test::RunAll;

// Expands inside the test body on purpose: the ASSERT_STREQ early-return
// must leave the case, not a helper function.
#define EXPECT_FMT(expected_text, buffer_capacity, format_text, ...)                               \
    do {                                                                                           \
        char format_buffer[buffer_capacity];                                                       \
        VformatToBuf(format_buffer, sizeof(format_buffer),                                         \
                     format_text __VA_OPT__(, ) __VA_ARGS__);                                      \
        ASSERT_STREQ(format_buffer, expected_text);                                                \
    } while (0)

TEST("format: plain text passes through") {
    EXPECT_FMT("hello, world", 64, "hello, world");
}

TEST("format: percent escape") {
    EXPECT_FMT("100%", 64, "100%%");
}

TEST("format: char") {
    EXPECT_FMT("x", 64, "%c", 'x');
}

TEST("format: string") {
    EXPECT_FMT("kernel", 64, "%s", "kernel");
}

TEST("format: null string is guarded") {
    EXPECT_FMT("(null)", 64, "%s", static_cast<const char*>(nullptr));
}

TEST("format: signed decimal") {
    EXPECT_FMT("42", 64, "%d", 42);
    EXPECT_FMT("-42", 64, "%d", -42);
    EXPECT_FMT("0", 64, "%d", 0);
}

TEST("format: unsigned decimal") {
    EXPECT_FMT("4294967295", 64, "%u", 4294967295U);
}

TEST("format: octal") {
    EXPECT_FMT("755", 64, "%o", 0755);
}

TEST("format: hex lower and upper") {
    EXPECT_FMT("deadbeef", 64, "%x", 0xDEADBEEFU);
    EXPECT_FMT("DEADBEEF", 64, "%X", 0xDEADBEEFU);
}

TEST("format: pointer gets 0x prefix") {
    char format_buffer[64];
    VformatToBuf(format_buffer, sizeof(format_buffer), "%p", reinterpret_cast<void*>(0x1234));
    ASSERT_STREQ(format_buffer, "0x1234");
}

TEST("format: long long modifiers hit 64-bit extremes") {
    EXPECT_FMT("-9223372036854775808", 64, "%lld", INT64_MIN);
    EXPECT_FMT("18446744073709551615", 64, "%llu", UINT64_MAX);
    EXPECT_FMT("ffffffffffffffff", 64, "%llx", UINT64_MAX);
}

TEST("format: z modifier reads 64-bit") {
    EXPECT_FMT("4096", 64, "%zu", static_cast<size_t>(4096));
    EXPECT_FMT("1000", 64, "%zx", static_cast<size_t>(0x1000));
}

TEST("format: width right-align space-pad") {
    EXPECT_FMT("       42", 64, "%9d", 42);
}

TEST("format: width zero-pad") {
    EXPECT_FMT("000000042", 64, "%09d", 42);
}

TEST("format: width left-align number") {
    EXPECT_FMT("42       ", 64, "%-9d", 42);
}

TEST("format: width left-align string") {
    EXPECT_FMT("ab      ", 64, "%-8s", "ab");
}

TEST("format: width right-align string") {
    EXPECT_FMT("      ab", 64, "%8s", "ab");
}

TEST("format: INT64_MIN does not overflow") {
    EXPECT_FMT("-9223372036854775808", 64, "%lld", INT64_MIN);
}

TEST("format: buffer truncation keeps NUL") {
    char format_buffer[5];
    VformatToBuf(format_buffer, sizeof(format_buffer), "%s", "abcdefghijklmn");
    ASSERT_STREQ(format_buffer, "abcd");
}

TEST("format: oversized output survives small buffer") {
    char format_buffer[8];
    VformatToBuf(format_buffer, sizeof(format_buffer), "%d %d %d", 111, 222, 333);
    ASSERT_STREQ(format_buffer, "111 222");
}

TEST("format: unknown specifier echoes verbatim") {
    EXPECT_FMT("%q", 64, "%q", 1);
}

int main() {
    return RunAll();
}
