#include "framework_kernel.hpp"
#include "kernel/driver/tty/tty.hpp"
#include "test_assert.hpp"
#include "test_case.hpp"  // NOLINT(misc-include-cleaner) consumed by the TEST() macro body

namespace {

/// Feeds the erasure study's key run and hands back the parked line.
unsigned long feed_erased_line(cinux::driver::Tty& tty, unsigned char* heard, unsigned long room) {
    tty.feed('e');
    tty.feed('c');
    tty.feed('g');
    tty.feed('\b');
    tty.feed(0x7F);
    tty.feed('h');
    tty.feed('o');
    tty.feed('\r');
    return tty.read_line(heard, room);
}

TEST("tty: echoes and parks one finished line") {
    cinux::driver::Tty& tty = cinux::driver::Tty::self();
    ASSERT_TRUE(!tty.feed('h'));
    ASSERT_TRUE(!tty.feed('i'));
    ASSERT_TRUE(tty.feed('\n'));
    unsigned char       heard[cinux::driver::kTtyLineMax] = {};
    const unsigned long kGot                              = tty.read_line(heard, sizeof(heard));
    ASSERT_EQ(kGot, 3UL);
    ASSERT_EQ(heard[0], static_cast<unsigned char>('h'));
    ASSERT_EQ(heard[1], static_cast<unsigned char>('i'));
    ASSERT_EQ(heard[2], static_cast<unsigned char>('\n'));
}

TEST("tty: an erasure eats the tail before the line parks") {
    cinux::driver::Tty& tty                               = cinux::driver::Tty::self();
    unsigned char       heard[cinux::driver::kTtyLineMax] = {};
    const unsigned long kGot = feed_erased_line(tty, heard, sizeof(heard));
    ASSERT_EQ(kGot, 4UL);
    ASSERT_EQ(heard[0], static_cast<unsigned char>('e'));
    ASSERT_EQ(heard[1], static_cast<unsigned char>('h'));
    ASSERT_EQ(heard[2], static_cast<unsigned char>('o'));
    ASSERT_EQ(heard[3], static_cast<unsigned char>('\n'));
}

TEST("tty: a raw key is input before any line exists") {
    cinux::driver::Tty& tty = cinux::driver::Tty::self();
    ASSERT_TRUE(!tty.has_input());
    ASSERT_TRUE(!tty.feed('x'));
    ASSERT_TRUE(tty.has_input());
    ASSERT_TRUE(tty.feed('\n'));
    unsigned char heard[cinux::driver::kTtyLineMax] = {};
    ASSERT_EQ(tty.read_line(heard, sizeof(heard)), 2UL);
    ASSERT_TRUE(!tty.has_input());
}

}  // namespace
