#include <termios.h>

#include <cstddef>
#include <cstring>

#include "../framework/framework.hpp"
#include "kernel/driver/tty/tty.hpp"
#include "kernel/driver/tty/tty_config.hpp"
#include "test_assert.hpp"

// The wire's kernel termios is thirty-six bytes; the libc struct beside
// it is larger and converted in library code, so its size proves
// nothing about the syscall record. The flag bits and the flag word's
// seat are ABI either way.
static_assert(cinux::driver::kTermiosBytes == 36, "kernel termios is 36 on the wire");
static_assert(offsetof(struct termios, c_lflag) == cinux::driver::kTermiosLflagAt,
              "the flag word sits at twelve in both shapes");
static_assert(ICANON == cinux::driver::kTermiosIcanon, "ICANON bit drifted");
static_assert(ECHO == cinux::driver::kTermiosEcho, "ECHO bit drifted");

namespace {

unsigned char g_echo_log[cinux::driver::kTtyLineMax] = {};
unsigned long g_echo_used                            = 0;

char          g_seat[cinux::driver::kTtyLineMax] = {};
unsigned long g_seat_used                        = 0;
unsigned long g_seat_read                        = 0;

void sink_echo(void* /*context*/, char key) {
    if (g_echo_used < cinux::driver::kTtyLineMax) {
        g_echo_log[g_echo_used] = static_cast<unsigned char>(key);
        ++g_echo_used;
    }
}

bool seat_takes(void* /*context*/, char* out) {
    if (g_seat_read >= g_seat_used) {
        return false;
    }
    *out = g_seat[g_seat_read];
    ++g_seat_read;
    return true;
}

bool seat_has(void* /*context*/) {
    return g_seat_read < g_seat_used;
}

void no_wait(void* /*context*/) {}

void reset_seat(const char* keys) {
    g_seat_used = 0;
    g_seat_read = 0;
    g_echo_used = 0;
    for (unsigned long index = 0; keys[index] != '\0'; ++index) {
        g_seat[g_seat_used] = keys[index];
        ++g_seat_used;
    }
}

const cinux::driver::TtyWiring kWiring{
    .context  = nullptr,
    .take_key = seat_takes,
    .has_key  = seat_has,
    .echo     = sink_echo,
    .wait     = no_wait,
};

unsigned int lflag_word(const unsigned char* record) {
    unsigned int word = 0;
    for (unsigned long index = 0; index < 4; ++index) {
        word |= static_cast<unsigned int>(record[cinux::driver::kTermiosLflagAt + index])
                << (8U * index);
    }
    return word;
}

}  // namespace

TEST("tty: a fresh terminal answers fully canonical termios") {
    const cinux::driver::Tty kTty(kWiring);
    unsigned char            record[cinux::driver::kTermiosBytes] = {};
    kTty.copy_termios(record);
    const unsigned int kFlags = lflag_word(record);
    ASSERT_EQ(kFlags & cinux::driver::kTermiosEcho, cinux::driver::kTermiosEcho);
    ASSERT_EQ(kFlags & cinux::driver::kTermiosIcanon, cinux::driver::kTermiosIcanon);
    ASSERT_EQ(kFlags & cinux::driver::kTermiosIsig, cinux::driver::kTermiosIsig);
    // The wire's c_cc starts at seventeen in both shapes, so the born
    // control characters read straight off the record without any libc
    // conversion.
    const auto kIntr = static_cast<unsigned>(record[17 + VINTR]);
    const auto kQuit = static_cast<unsigned>(record[17 + VQUIT]);
    const auto kSusp = static_cast<unsigned>(record[17 + VSUSP]);
    ASSERT_EQ(kIntr, 0x03U);
    ASSERT_EQ(kQuit, 0x1CU);
    ASSERT_EQ(kSusp, 0x1AU);
}

TEST("tty: a fallen ECHO bit silences the editor's echo") {
    unsigned char record[cinux::driver::kTermiosBytes] = {};
    {
        const cinux::driver::Tty kBorn(kWiring);
        kBorn.copy_termios(record);
    }
    record[cinux::driver::kTermiosLflagAt] = static_cast<unsigned char>(
        record[cinux::driver::kTermiosLflagAt] & ~cinux::driver::kTermiosEcho);

    reset_seat("");
    cinux::driver::Tty tty(kWiring);
    tty.apply_termios(record);
    ASSERT_TRUE(!tty.feed('h'));
    ASSERT_TRUE(!tty.feed('i'));
    ASSERT_TRUE(tty.feed('\n'));
    ASSERT_EQ(g_echo_used, 0UL);
}

TEST("tty: a fallen ICANON bit passes bytes through unedited") {
    unsigned char record[cinux::driver::kTermiosBytes] = {};
    {
        const cinux::driver::Tty kBorn(kWiring);
        kBorn.copy_termios(record);
    }
    record[cinux::driver::kTermiosLflagAt] = static_cast<unsigned char>(
        record[cinux::driver::kTermiosLflagAt] & ~cinux::driver::kTermiosIcanon);

    reset_seat("ab");
    cinux::driver::Tty tty(kWiring);
    tty.apply_termios(record);
    unsigned char       heard[8] = {};
    const unsigned long kGot     = tty.read_line(heard, sizeof(heard));
    ASSERT_EQ(kGot, 1UL);
    ASSERT_EQ(heard[0], static_cast<unsigned char>('a'));
}

TEST("tty: an applied record answers back verbatim") {
    unsigned char record[cinux::driver::kTermiosBytes] = {};
    for (unsigned long index = 0; index < cinux::driver::kTermiosBytes; ++index) {
        record[index] = static_cast<unsigned char>(index * 3U);
    }
    record[cinux::driver::kTermiosLflagAt] = 0x0BU;

    cinux::driver::Tty tty(kWiring);
    tty.apply_termios(record);
    unsigned char back[cinux::driver::kTermiosBytes] = {};
    tty.copy_termios(back);
    ASSERT_EQ(std::memcmp(record, back, cinux::driver::kTermiosBytes), 0);
}

TEST("tty: noncanonical ISIG consumes signal bytes without leaking them to read") {
    reset_seat("\x03");
    cinux::driver::Tty tty(kWiring);
    struct termios     record{};
    tty.copy_termios(reinterpret_cast<unsigned char*>(&record));
    record.c_lflag = ISIG;
    tty.apply_termios(reinterpret_cast<const unsigned char*>(&record));
    unsigned char heard = 0;
    ASSERT_EQ(tty.read_line(&heard, 1), 0UL);
    ASSERT_EQ(tty.take_pending_signal(), 2);
    ASSERT_FALSE(tty.has_line());
    ASSERT_EQ(g_echo_used, 0UL);
}

TEST("tty: ash raw settings pass VINTR as data without kernel echo or signals") {
    reset_seat("\x03");
    cinux::driver::Tty tty(kWiring);
    struct termios     record{};
    tty.copy_termios(reinterpret_cast<unsigned char*>(&record));
    record.c_lflag = 0;
    tty.apply_termios(reinterpret_cast<const unsigned char*>(&record));
    unsigned char heard = 0;
    ASSERT_EQ(tty.read_line(&heard, 1), 1UL);
    ASSERT_EQ(heard, record.c_cc[VINTR]);
    ASSERT_EQ(tty.take_pending_signal(), 0);
    ASSERT_EQ(g_echo_used, 0UL);
}

TEST("tty: VINTR may be remapped or disabled") {
    reset_seat("");
    cinux::driver::Tty tty(kWiring);
    struct termios     record{};
    tty.copy_termios(reinterpret_cast<unsigned char*>(&record));
    record.c_cc[VINTR] = 'x';
    tty.apply_termios(reinterpret_cast<const unsigned char*>(&record));
    tty.feed('x');
    ASSERT_EQ(tty.take_pending_signal(), 2);
    record.c_cc[VINTR] = 0;
    record.c_lflag     = ISIG;
    tty.apply_termios(reinterpret_cast<const unsigned char*>(&record));
    tty.feed('\0');
    unsigned char heard = 99;
    ASSERT_EQ(tty.read_line(&heard, 1), 1UL);
    ASSERT_EQ(heard, static_cast<unsigned char>(0));
    ASSERT_EQ(tty.take_pending_signal(), 0);
}

TEST("tty: repeated signal keys stay consumed and flush the abandoned line") {
    reset_seat("");
    cinux::driver::Tty tty(kWiring);
    tty.feed('x');
    tty.feed('\x03');
    tty.feed('\x03');
    ASSERT_EQ(tty.take_pending_signal(), 2);
    tty.feed('y');
    tty.feed('\n');
    unsigned char heard[8] = {};
    ASSERT_EQ(tty.read_line(heard, sizeof(heard)), 2UL);
    ASSERT_EQ(heard[0], static_cast<unsigned char>('y'));
}

int main() {
    return cinux::test::RunAll();
}
