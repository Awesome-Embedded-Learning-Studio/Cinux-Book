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

int main() {
    return cinux::test::RunAll();
}
