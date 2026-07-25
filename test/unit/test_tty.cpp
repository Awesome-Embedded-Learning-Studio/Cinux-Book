/**
 * @file test/unit/test_tty.cpp
 * @brief Host unit tests for the TTY line discipline (F10-M3 batch 1)
 *
 * Links the real kernel/drivers/tty/tty.cpp (no mocks) and exercises the
 * canonical line discipline: echo, backspace editing, ^C/^D/^U, and raw mode.
 * Self-contained (own asserts + main) so it runs as a plain ctest binary.
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "kernel/drivers/tty/tty.hpp"

using cinux::drivers::InputResult;
using cinux::drivers::TTY;
using cinux::drivers::Termios;
using cinux::drivers::TtySignal;
using cinux::drivers::kCharBackspace;
using cinux::drivers::kCharEof;
using cinux::drivers::kCharIntr;
using cinux::drivers::kEcho;
using cinux::drivers::kIcanon;
using cinux::drivers::kIsig;
using cinux::drivers::kVeof;
using cinux::drivers::kVintr;
using cinux::drivers::kVerase;
using cinux::drivers::make_default_termios;

static int g_fail = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);                                    \
            g_fail = 1;                                                                            \
        }                                                                                          \
    } while (0)

#define CHECK_EQ(a, b)                                                                             \
    do {                                                                                           \
        if ((a) != (b)) {                                                                          \
            std::printf("FAIL line %d: %s != %s\n", __LINE__, #a, #b);                             \
            g_fail = 1;                                                                            \
        }                                                                                          \
    } while (0)

// echo capture sink
static char   g_echo[256];
static size_t g_echo_len;
static void   echo_capture(char c, void* /*ctx*/) {
    if (g_echo_len < sizeof(g_echo)) {
        g_echo[g_echo_len++] = c;
    }
}
static void echo_reset() {
    g_echo_len = 0;
}

int main() {
    // Test 1: default termios is cooked (ICANON|ECHO|ISIG) with the right cc.
    {
        TTY            t;
        const Termios& tm = t.termios();
        CHECK(tm.c_lflag & kIcanon);
        CHECK(tm.c_lflag & kEcho);
        CHECK(tm.c_lflag & kIsig);
        CHECK_EQ(tm.c_cc[kVintr], kCharIntr);
        CHECK_EQ(tm.c_cc[kVeof], kCharEof);
        CHECK_EQ(tm.c_cc[kVerase], kCharBackspace);
    }

    // Test 2: canonical line accumulation + echo.
    {
        TTY t;
        t.set_echo_sink(echo_capture, nullptr);
        echo_reset();
        CHECK_EQ(t.input_char('a'), InputResult::kConsumed);
        CHECK_EQ(t.input_char('b'), InputResult::kConsumed);
        CHECK_EQ(t.input_char('\n'), InputResult::kLineReady);
        CHECK_EQ(g_echo_len, static_cast<size_t>(3));
        CHECK(g_echo[0] == 'a' && g_echo[1] == 'b' && g_echo[2] == '\n');
        char   buf[16];
        size_t n = t.read_cooked(buf, sizeof(buf));
        CHECK_EQ(n, static_cast<size_t>(3));
        CHECK(buf[0] == 'a' && buf[1] == 'b' && buf[2] == '\n');
        // queue drained
        CHECK_EQ(t.read_cooked(buf, sizeof(buf)), static_cast<size_t>(0));
    }

    // Test 3: backspace editing (VERASE = DEL).
    {
        TTY t;
        t.set_echo_sink(echo_capture, nullptr);
        echo_reset();
        t.input_char('a');
        t.input_char('b');
        t.input_char(kCharBackspace);  // erase 'b'
        t.input_char('d');
        CHECK_EQ(t.input_char('\n'), InputResult::kLineReady);
        char   buf[16];
        size_t n = t.read_cooked(buf, sizeof(buf));
        CHECK_EQ(n, static_cast<size_t>(3));
        CHECK(buf[0] == 'a' && buf[1] == 'd' && buf[2] == '\n');
    }

    // Test 4: ^C generates SIGINT (never reaches the line).
    {
        TTY t;
        CHECK_EQ(t.input_char(kCharIntr), InputResult::kSignal);
        CHECK_EQ(t.pending_signal(), TtySignal::kSigint);
        char buf[16];
        CHECK_EQ(t.read_cooked(buf, sizeof(buf)), static_cast<size_t>(0));  // no line
    }

    // Test 5: ^D on an empty line -> EOF; take_eof() signals it once.
    {
        TTY t;
        CHECK_EQ(t.input_char(kCharEof), InputResult::kEof);
        CHECK(t.take_eof());
        CHECK(!t.take_eof());  // consumed -- EOF delivered exactly once
    }

    // Test 6: ^D after input commits the line WITHOUT a trailing newline.
    {
        TTY t;
        t.input_char('x');
        t.input_char('y');
        CHECK_EQ(t.input_char(kCharEof), InputResult::kLineReady);
        char   buf[16];
        size_t n = t.read_cooked(buf, sizeof(buf));
        CHECK_EQ(n, static_cast<size_t>(2));
        CHECK(buf[0] == 'x' && buf[1] == 'y');
    }

    // Test 7: ^U kills the line being edited.
    {
        TTY t;
        t.input_char('a');
        t.input_char('b');
        t.input_char('c');
        t.input_char(0x15);  // ^U (VKILL default)
        t.input_char('z');
        CHECK_EQ(t.input_char('\n'), InputResult::kLineReady);
        char   buf[16];
        size_t n = t.read_cooked(buf, sizeof(buf));
        CHECK_EQ(n, static_cast<size_t>(2));
        CHECK(buf[0] == 'z' && buf[1] == '\n');
    }

    // Test 8: raw mode (~ICANON) passes each byte straight through.
    {
        TTY     t;
        Termios tm;
        make_default_termios(tm);
        tm.c_lflag &= ~kIcanon;
        t.set_termios(tm);
        CHECK_EQ(t.input_char('x'), InputResult::kLineReady);
        CHECK_EQ(t.input_char('y'), InputResult::kLineReady);
        char   buf[16];
        size_t n = t.read_cooked(buf, sizeof(buf));
        CHECK_EQ(n, static_cast<size_t>(2));
        CHECK(buf[0] == 'x' && buf[1] == 'y');
    }

    // Test 9: overflow drops the extra char (line buffer cap).
    {
        TTY  t;
        bool saw_overflow = false;
        for (int i = 0; i < 400; i++) {
            if (t.input_char('a' + (i % 26)) == InputResult::kOverflow) {
                saw_overflow = true;
            }
        }
        CHECK(saw_overflow);
    }

    if (g_fail) {
        std::printf("tty tests FAILED\n");
        return 1;
    }
    std::printf("tty tests OK (9 cases)\n");
    return 0;
}
