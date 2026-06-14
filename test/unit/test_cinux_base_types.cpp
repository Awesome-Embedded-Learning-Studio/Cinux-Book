/**
 * @file test/unit/test_cinux_base_types.cpp
 * @brief Smoke test: prove Cinux-Base types compile & work under the kernel include path
 *
 * Cinux-Base (third_party/Cinux-Base/) is the header-only cinux::lib type library
 * the kernel was just wired into. Before migrating any kernel code onto it, this
 * test verifies that the leaf types (ErrorOr / StringView / Span / Buffer) are
 * usable from the host test build. Only header-only types are exercised here so
 * the test needs no extra .cpp to link against.
 *
 * Compile condition: -DCINUX_HOST_TEST
 */

#define TEST_FRAMEWORK_IMPL
#include "test_framework.h"

#include <cinux/buffer.hpp>
#include <cinux/expected.hpp>
#include <cinux/span.hpp>
#include <cinux/string_view.hpp>

using cinux::lib::BufferView;
using cinux::lib::ByteSpan;
using cinux::lib::Error;
using cinux::lib::ErrorOr;
using cinux::lib::Span;
using cinux::lib::StaticBuffer;
using cinux::lib::StringView;

// ============================================================
// ErrorOr<T>
// ============================================================

TEST("cinux_base: ErrorOr success path") {
    ErrorOr<int> result = 42;

    ASSERT_TRUE(result.ok());
    ASSERT_TRUE(static_cast<bool>(result));
    ASSERT_EQ(result.value(), 42);
    ASSERT_EQ(*result, 42);
}

TEST("cinux_base: ErrorOr error path") {
    ErrorOr<int> result = Error::NotFound;

    ASSERT_FALSE(result.ok());
    ASSERT_FALSE(static_cast<bool>(result));
    ASSERT_EQ(result.error(), Error::NotFound);
}

TEST("cinux_base: ErrorOr<void> specialization") {
    ErrorOr<void> ok;                       // default = success
    ASSERT_TRUE(ok.ok());
    ASSERT_EQ(ok.error(), Error::Ok);

    ErrorOr<void> err = Error::OutOfMemory;
    ASSERT_FALSE(err.ok());
    ASSERT_EQ(err.error(), Error::OutOfMemory);
}

TEST("cinux_base: error_string covers enum values") {
    ASSERT_TRUE(StringView(cinux::lib::error_string(Error::Ok)).equals("Ok"));
    ASSERT_TRUE(
        StringView(cinux::lib::error_string(Error::NotFound)).equals("NotFound"));
}

// ============================================================
// StringView
// ============================================================

TEST("cinux_base: StringView construction & accessors") {
    StringView sv("hello");

    ASSERT_EQ(sv.size(), 5u);
    ASSERT_FALSE(sv.empty());
    ASSERT_EQ(sv.front(), 'h');
    ASSERT_EQ(sv.back(), 'o');
    ASSERT_EQ(sv[1], 'e');

    StringView empty;
    ASSERT_TRUE(empty.empty());
    ASSERT_EQ(empty.size(), 0u);
}

TEST("cinux_base: StringView search & substring") {
    StringView sv("foo/bar/baz");

    ASSERT_EQ(sv.find('/'), 3u);
    ASSERT_EQ(sv.rfind('/'), 7u);
    ASSERT_TRUE(sv.starts_with("foo"));
    ASSERT_TRUE(sv.ends_with("baz"));

    // Substring up to the last separator gives the parent path.
    StringView parent = sv.substr(0, sv.rfind('/'));
    ASSERT_TRUE(parent.equals("foo/bar"));

    // Not found returns npos.
    ASSERT_EQ(sv.find('z'), 10u);
    ASSERT_EQ(sv.find('z', 11), StringView::npos);
}

TEST("cinux_base: StringView comparison operators") {
    StringView a("abc");
    StringView b("abc");
    StringView c("abd");

    ASSERT_TRUE(a == b);
    ASSERT_FALSE(a != b);
    ASSERT_TRUE(a < c);
    ASSERT_TRUE(c > a);
}

// ============================================================
// Span<T>
// ============================================================

TEST("cinux_base: Span from C array & iteration") {
    int arr[] = {1, 2, 3, 4};
    Span<int> s(arr);

    ASSERT_EQ(s.size(), 4u);
    ASSERT_EQ(s[0], 1);
    ASSERT_EQ(s.front(), 1);
    ASSERT_EQ(s.back(), 4);

    int sum = 0;
    for (int x : s) {
        sum += x;
    }
    ASSERT_EQ(sum, 10);
}

TEST("cinux_base: Span subviews") {
    int arr[] = {10, 20, 30, 40};
    Span<int> s(arr);

    Span<int> first_two = s.first(2);
    ASSERT_EQ(first_two.size(), 2u);
    ASSERT_EQ(first_two[1], 20);

    Span<int> last_one = s.last(1);
    ASSERT_EQ(last_one.size(), 1u);
    ASSERT_EQ(last_one[0], 40);

    Span<int> mid = s.subspan(1, 2);
    ASSERT_EQ(mid.size(), 2u);
    ASSERT_EQ(mid[0], 20);
    ASSERT_EQ(mid[1], 30);
}

TEST("cinux_base: ByteSpan alias") {
    uint8_t bytes[] = {0xDE, 0xAD, 0xBE, 0xEF};
    ByteSpan b(bytes);

    ASSERT_EQ(b.size(), 4u);
    ASSERT_EQ(b[0], 0xDE);
    ASSERT_EQ(b[3], 0xEF);
}

// ============================================================
// Buffer (BufferView + StaticBuffer<N>)
// ============================================================

TEST("cinux_base: StaticBuffer copy & access") {
    StaticBuffer<16> buf;
    buf.copy_from("hello", 5);

    ASSERT_EQ(buf.size(), 5u);
    ASSERT_EQ(buf.data()[0], static_cast<uint8_t>('h'));

    char out[8] = {0};
    buf.copy_to(out, 5);
    ASSERT_EQ(out[0], 'h');
    ASSERT_EQ(out[4], 'o');
}

TEST("cinux_base: StaticBuffer fill & view") {
    StaticBuffer<8> buf;
    buf.fill(0xAB);

    ASSERT_EQ(buf.size(), 8u);
    ASSERT_EQ(buf.capacity(), 8u);
    for (size_t i = 0; i < buf.size(); ++i) {
        ASSERT_EQ(buf.data()[i], 0xAB);
    }

    BufferView view = buf.view();
    ASSERT_EQ(view.size(), 8u);
    ASSERT_EQ(view[0], 0xAB);
}

TEST("cinux_base: BufferView slice") {
    uint8_t raw[] = {0, 1, 2, 3, 4, 5};
    BufferView view(raw, 6);

    BufferView mid = view.slice(2, 2);
    ASSERT_EQ(mid.size(), 2u);
    ASSERT_EQ(mid[0], 2);
    ASSERT_EQ(mid[1], 3);

    // Slice past the end is safely clamped.
    BufferView over = view.slice(5, 10);
    ASSERT_EQ(over.size(), 1u);
}

// ============================================================
// Main
// ============================================================

int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}
