/**
 * @file test/unit/test_kprintf.cpp
 * @brief Host-side unit tests for the big kernel vkprintf_impl engine
 *
 * Includes the header-only vkprintf_impl.hpp and supplies a mock
 * OutputFn that captures characters into a std::string.
 */

#define TEST_FRAMEWORK_IMPL
#include "test_framework.h"

#include <string>
#include <stdarg.h>

#include "kernel/lib/private/vkprintf_impl.hpp"

using cinux::lib::detail::vkprintf_impl;

namespace {

std::string g_output;

static void test_putc(char c) { g_output += c; }

static std::string do_fmt(const char* fmt, ...) {
    g_output.clear();
    va_list ap;
    va_start(ap, fmt);
    vkprintf_impl(test_putc, fmt, ap);
    va_end(ap);
    return g_output;
}

}  // anonymous namespace

// ============================================================
// Basic specifiers
// ============================================================

TEST("kprintf: %% produces literal %") {
    ASSERT_EQ(do_fmt("%%"), std::string("%"));
}

TEST("kprintf: %c outputs character") {
    ASSERT_EQ(do_fmt("%c", 'Z'), std::string("Z"));
}

TEST("kprintf: %s outputs string") {
    ASSERT_EQ(do_fmt("%s", "hello"), std::string("hello"));
}

TEST("kprintf: %s nullptr shows (null)") {
    ASSERT_EQ(do_fmt("%s", static_cast<const char*>(nullptr)), std::string("(null)"));
}

TEST("kprintf: %d positive") {
    ASSERT_EQ(do_fmt("%d", 42), std::string("42"));
}

TEST("kprintf: %d negative") {
    ASSERT_EQ(do_fmt("%d", -123), std::string("-123"));
}

TEST("kprintf: %d zero") {
    ASSERT_EQ(do_fmt("%d", 0), std::string("0"));
}

TEST("kprintf: %u unsigned") {
    ASSERT_EQ(do_fmt("%u", 4294967295u), std::string("4294967295"));
}

TEST("kprintf: %x lowercase hex") {
    ASSERT_EQ(do_fmt("%x", 0xdeadbeefULL), std::string("deadbeef"));
}

TEST("kprintf: %X uppercase hex") {
    ASSERT_EQ(do_fmt("%X", 0xDEADBEEFULL), std::string("DEADBEEF"));
}

TEST("kprintf: %p pointer format") {
    ASSERT_EQ(do_fmt("%p", 0xFFFFFFFF80000000ULL),
              std::string("0xFFFFFFFF80000000"));
}

// ============================================================
// Width / padding
// ============================================================

TEST("kprintf: %5d right-align space-pad") {
    ASSERT_EQ(do_fmt("%5d", 7), std::string("    7"));
}

TEST("kprintf: %05d zero-pad") {
    ASSERT_EQ(do_fmt("%05d", 7), std::string("00007"));
}

TEST("kprintf: %-5d left-align") {
    ASSERT_EQ(do_fmt("%-5d", 7), std::string("7    "));
}

TEST("kprintf: %08x zero-pad hex") {
    ASSERT_EQ(do_fmt("%08x", 0xFFULL), std::string("000000ff"));
}

TEST("kprintf: %-10s left-align string") {
    ASSERT_EQ(do_fmt("%-10s!", "hi"), std::string("hi        !"));
}

// ============================================================
// Combined / edge cases
// ============================================================

TEST("kprintf: mixed specifiers") {
    ASSERT_EQ(do_fmt("%s %d %x", "test", -5, 0xABULL),
              std::string("test -5 ab"));
}

TEST("kprintf: unknown specifier passes through") {
    ASSERT_EQ(do_fmt("%q"), std::string("%q"));
}

TEST("kprintf: empty format") {
    ASSERT_EQ(do_fmt(""), std::string(""));
}

TEST("kprintf: plain text") {
    ASSERT_EQ(do_fmt("hello world"), std::string("hello world"));
}

// ============================================================
// Main
// ============================================================

int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}
