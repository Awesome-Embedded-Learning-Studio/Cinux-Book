#include "../framework/framework.hpp"
#include "kernel/driver/tty/pty.hpp"
#include "kernel/driver/tty/tty_config.hpp"
#include "test_assert.hpp"

namespace {

void no_wait(void* /*context*/) {}

unsigned int lflag_word(const unsigned char* record) {
    unsigned int word = 0;
    for (unsigned long index = 0; index < 4; ++index) {
        word |= static_cast<unsigned int>(record[cinux::driver::kTermiosLflagAt + index])
                << (8U * index);
    }
    return word;
}

}  // namespace

TEST("pty: typed keys cross to the slave as an edited line") {
    cinux::driver::Pty  pair(&no_wait);
    const unsigned long kFed = pair.master_write("hi\n", 3);
    ASSERT_EQ(kFed, 3UL);

    unsigned char       heard[16] = {};
    const unsigned long kGot      = pair.slave().read_line(heard, sizeof(heard));
    ASSERT_EQ(kGot, 3UL);
    ASSERT_EQ(heard[0], static_cast<unsigned char>('h'));
    ASSERT_EQ(heard[1], static_cast<unsigned char>('i'));
    ASSERT_EQ(heard[2], static_cast<unsigned char>('\n'));
}

TEST("pty: the discipline's echo flows back to the master") {
    cinux::driver::Pty pair(&no_wait);
    pair.master_write("hi\n", 3);
    unsigned char heard[16] = {};
    static_cast<void>(pair.slave().read_line(heard, sizeof(heard)));

    unsigned char       seen[16] = {};
    const unsigned long kEcho    = pair.master_read(seen, sizeof(seen));
    ASSERT_EQ(kEcho, 3UL);
    ASSERT_EQ(seen[0], static_cast<unsigned char>('h'));
    ASSERT_EQ(seen[1], static_cast<unsigned char>('i'));
    ASSERT_EQ(seen[2], static_cast<unsigned char>('\n'));
}

TEST("pty: the slave's own writes reach the master, not itself") {
    cinux::driver::Pty  pair(&no_wait);
    const unsigned long kPushed = pair.slave_write("out", 3);
    ASSERT_EQ(kPushed, 3UL);
    ASSERT_TRUE(!pair.slave().has_line());

    unsigned char       seen[16] = {};
    const unsigned long kGot     = pair.master_read(seen, sizeof(seen));
    ASSERT_EQ(kGot, 3UL);
    ASSERT_EQ(seen[0], static_cast<unsigned char>('o'));
}

TEST("pty: a fallen ECHO bit silences the pair's echo only") {
    cinux::driver::Pty pair(&no_wait);
    unsigned char      record[cinux::driver::kTermiosBytes] = {};
    pair.slave().copy_termios(record);
    record[cinux::driver::kTermiosLflagAt] = static_cast<unsigned char>(
        record[cinux::driver::kTermiosLflagAt] & ~cinux::driver::kTermiosEcho);
    pair.slave().apply_termios(record);

    pair.master_write("hi\n", 3);
    unsigned char heard[16] = {};
    ASSERT_EQ(pair.slave().read_line(heard, sizeof(heard)), 3UL);
    ASSERT_TRUE(!pair.master_has_input());
}

TEST("pty: a fallen ICANON bit hands the slave single bytes") {
    cinux::driver::Pty pair(&no_wait);
    unsigned char      record[cinux::driver::kTermiosBytes] = {};
    pair.slave().copy_termios(record);
    record[cinux::driver::kTermiosLflagAt] = static_cast<unsigned char>(
        record[cinux::driver::kTermiosLflagAt] & ~cinux::driver::kTermiosIcanon);
    pair.slave().apply_termios(record);

    pair.master_write("ab", 2);
    unsigned char       heard[8] = {};
    const unsigned long kGot     = pair.slave().read_line(heard, sizeof(heard));
    ASSERT_EQ(kGot, 1UL);
    ASSERT_EQ(heard[0], static_cast<unsigned char>('a'));
}

TEST("pty: reset drains both rings and restores born flags") {
    cinux::driver::Pty pair(&no_wait);
    pair.master_write("x", 1);
    pair.slave_write("y", 1);
    pair.reset();
    ASSERT_TRUE(!pair.master_has_input());
    ASSERT_TRUE(!pair.raw_has_keys());

    unsigned char record[cinux::driver::kTermiosBytes] = {};
    pair.slave().copy_termios(record);
    const unsigned int kFlags = lflag_word(record);
    ASSERT_EQ(kFlags & cinux::driver::kTermiosEcho, cinux::driver::kTermiosEcho);
}

TEST("pty: master hangup drains a final partial canonical line then EOF") {
    cinux::driver::Pty pair(&no_wait);
    pair.retain_master();
    pair.master_write("tail", 4);
    unsigned char heard[8] = {};
    ASSERT_EQ(pair.slave().read_line(heard, sizeof(heard), false), 0UL);
    pair.release_master();
    ASSERT_TRUE(pair.slave().has_input());
    ASSERT_EQ(pair.slave().read_line(heard, 2), 2UL);
    ASSERT_EQ(heard[0], static_cast<unsigned char>('t'));
    ASSERT_EQ(heard[1], static_cast<unsigned char>('a'));
    ASSERT_EQ(pair.slave().read_line(heard, sizeof(heard)), 2UL);
    ASSERT_EQ(heard[0], static_cast<unsigned char>('i'));
    ASSERT_EQ(pair.slave().read_line(heard, sizeof(heard)), 0UL);
}

TEST("pty: the first slave open differs from the last slave close") {
    cinux::driver::Pty pair(&no_wait);
    ASSERT_FALSE(pair.master_has_input());
    pair.retain_slave();
    pair.retain_slave();
    pair.release_slave();
    ASSERT_FALSE(pair.slave_closed());
    pair.slave_write("out", 3);
    pair.release_slave();
    ASSERT_TRUE(pair.master_has_input());
    unsigned char heard[8] = {};
    ASSERT_EQ(pair.master_read(heard, sizeof(heard)), 3UL);
    ASSERT_TRUE(pair.master_has_input());
    ASSERT_EQ(pair.master_read(heard, sizeof(heard)), 0UL);
}

TEST("pty: reopening a slave clears EOF and reset restores the unopened state") {
    cinux::driver::Pty pair(&no_wait);
    pair.retain_slave();
    pair.release_slave();
    pair.retain_slave();
    ASSERT_FALSE(pair.master_has_input());
    pair.release_slave();
    pair.reset();
    ASSERT_FALSE(pair.master_has_input());
    ASSERT_FALSE(pair.master_closed());
}

namespace {

cinux::driver::Pty* g_closing_pair = nullptr;

void close_waiting_master(void* context) {
    auto* const kPair = static_cast<cinux::driver::Pty*>(context);
    g_closing_pair    = kPair;
    kPair->release_master();
}

}  // namespace

TEST("pty: master close releases a slave already waiting inside the discipline") {
    cinux::driver::Pty pair(&close_waiting_master);
    pair.retain_master();
    unsigned char heard = 0;
    g_closing_pair      = nullptr;
    ASSERT_EQ(pair.slave().read_line(&heard, 1), 0UL);
    ASSERT_TRUE(g_closing_pair == &pair);
    ASSERT_TRUE(pair.master_closed());
}

int main() {
    return cinux::test::RunAll();
}
