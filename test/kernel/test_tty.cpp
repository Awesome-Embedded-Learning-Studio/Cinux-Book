#include "framework_kernel.hpp"
#include "kernel/driver/tty/tty.hpp"
#include "kernel/driver/tty/tty_config.hpp"
#include "test_assert.hpp"
#include "test_case.hpp"  // NOLINT(misc-include-cleaner) consumed by the TEST() macro body

namespace {

/// Where the discipline's echo lands in this world, so cases can read
/// back what a terminal showed.
unsigned char g_echo_log[cinux::driver::kTtyLineMax] = {};
unsigned long g_echo_used                            = 0;

void sink_echo(void* /*context*/, char key) {
    if (g_echo_used < cinux::driver::kTtyLineMax) {
        g_echo_log[g_echo_used] = static_cast<unsigned char>(key);
        ++g_echo_used;
    }
}

bool never_takes(void* /*context*/, char* /*out*/) {
    return false;
}
bool never_has(void* /*context*/) {
    return false;
}
void no_wait(void* /*context*/) {}

const cinux::driver::TtyWiring kWiring{
    .context  = nullptr,
    .take_key = never_takes,
    .has_key  = never_has,
    .echo     = sink_echo,
    .wait     = no_wait,
};

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

}  // namespace

TEST("tty: echoes and parks one finished line") {
    cinux::driver::Tty tty(kWiring);
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
    cinux::driver::Tty  tty(kWiring);
    unsigned char       heard[cinux::driver::kTtyLineMax] = {};
    const unsigned long kGot = feed_erased_line(tty, heard, sizeof(heard));
    ASSERT_EQ(kGot, 4UL);
    ASSERT_EQ(heard[0], static_cast<unsigned char>('e'));
    ASSERT_EQ(heard[1], static_cast<unsigned char>('h'));
    ASSERT_EQ(heard[2], static_cast<unsigned char>('o'));
    ASSERT_EQ(heard[3], static_cast<unsigned char>('\n'));
}

TEST("tty: a raw key is input before any line exists") {
    cinux::driver::Tty tty(kWiring);
    ASSERT_TRUE(!tty.has_input());
    ASSERT_TRUE(!tty.feed('x'));
    ASSERT_TRUE(tty.has_input());
    ASSERT_TRUE(tty.feed('\n'));
    unsigned char heard[cinux::driver::kTtyLineMax] = {};
    ASSERT_EQ(tty.read_line(heard, sizeof(heard)), 2UL);
    ASSERT_TRUE(!tty.has_input());
}
