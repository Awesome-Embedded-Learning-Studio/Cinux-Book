/**
 * @file test/unit/test_pty.cpp
 * @brief Host unit tests for the PTY master/slave pair (F10-M3 Phase 2 batch 1)
 *
 * Links the real kernel/drivers/tty/{pty,tty}.cpp (no mocks) and exercises the
 * four data paths: master->slave (cooked line + echo), slave->master (output),
 * signal chars, raw mode, editing, EOF, and partial write on a full ring.
 * Self-contained (own asserts + main) so it runs as a plain ctest binary.
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "kernel/drivers/tty/pty.hpp"

using cinux::drivers::Pty;
using cinux::drivers::TtySignal;
using cinux::drivers::kCharBackspace;
using cinux::drivers::kCharEof;
using cinux::drivers::kCharIntr;
using cinux::drivers::kIcanon;

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

int main() {
    // Test 1: master types a canonical line -> slave reads it cooked.
    {
        Pty        p;
        const char input[] = "hi\n";
        auto       mw      = p.master_write(input, 3);
        CHECK(mw.ok());
        CHECK_EQ(*mw, static_cast<int64_t>(3));
        char buf[16];
        auto sr = p.slave_read(buf, sizeof(buf));
        CHECK(sr.ok());
        CHECK_EQ(*sr, static_cast<int64_t>(3));
        CHECK(buf[0] == 'h' && buf[1] == 'i' && buf[2] == '\n');
    }

    // Test 2: local echo routes to the master read side (emulator sees typing).
    {
        Pty  p;
        char echo[16];
        CHECK(p.master_write("ab\n", 3).ok());
        auto mr = p.master_read(echo, sizeof(echo));
        CHECK(mr.ok());
        CHECK_EQ(*mr, static_cast<int64_t>(3));
        CHECK(echo[0] == 'a' && echo[1] == 'b' && echo[2] == '\n');
    }

    // Test 3: slave write (program output) -> master read.
    {
        Pty        p;
        const char out[] = "output";
        auto       sw    = p.slave_write(out, 6);
        CHECK(sw.ok());
        CHECK_EQ(*sw, static_cast<int64_t>(6));
        char buf[16];
        auto mr = p.master_read(buf, sizeof(buf));
        CHECK(mr.ok());
        CHECK_EQ(*mr, static_cast<int64_t>(6));
        CHECK(std::memcmp(buf, "output", 6) == 0);
    }

    // Test 4: signal char on master -> pending SIGINT, no line committed.
    {
        Pty p;
        CHECK(p.master_write(&kCharIntr, 1).ok());
        CHECK_EQ(p.pending_signal(), TtySignal::kSigint);
        char buf[16];
        auto sr = p.slave_read(buf, sizeof(buf));
        CHECK(sr.ok());
        CHECK_EQ(*sr, static_cast<int64_t>(0));  // no line
        CHECK_EQ(p.take_pending_signal(), TtySignal::kSigint);
        CHECK_EQ(p.pending_signal(), TtySignal::kNone);  // consumed
    }

    // Test 5: raw mode (~ICANON) -> each master byte is immediately readable.
    {
        Pty  p;
        auto tm = p.slave_tty().termios();
        tm.c_lflag &= ~kIcanon;
        p.slave_tty().set_termios(tm);
        CHECK(p.master_write("x", 1).ok());
        char buf[16];
        auto sr = p.slave_read(buf, sizeof(buf));
        CHECK(sr.ok());
        CHECK_EQ(*sr, static_cast<int64_t>(1));
        CHECK(buf[0] == 'x');
    }

    // Test 6: backspace editing through the pair.
    {
        Pty p;
        CHECK(p.master_write("ab", 2).ok());
        CHECK(p.master_write(&kCharBackspace, 1).ok());  // erase 'b'
        CHECK(p.master_write("d\n", 2).ok());
        char buf[16];
        auto sr = p.slave_read(buf, sizeof(buf));
        CHECK(sr.ok());
        CHECK_EQ(*sr, static_cast<int64_t>(3));
        CHECK(buf[0] == 'a' && buf[1] == 'd' && buf[2] == '\n');
    }

    // Test 7: ^D on an empty line -> slave read 0 + EOF flag set once.
    {
        Pty p;
        CHECK(p.master_write(&kCharEof, 1).ok());
        char buf[16];
        auto sr = p.slave_read(buf, sizeof(buf));
        CHECK(sr.ok());
        CHECK_EQ(*sr, static_cast<int64_t>(0));
        CHECK(p.slave_tty().take_eof());   // EOF signalled
        CHECK(!p.slave_tty().take_eof());  // exactly once
    }

    // Test 8: ^D after buffered input commits the line (no EOF, no newline).
    {
        Pty p;
        CHECK(p.master_write("xy", 2).ok());
        CHECK(p.master_write(&kCharEof, 1).ok());
        char buf[16];
        auto sr = p.slave_read(buf, sizeof(buf));
        CHECK(sr.ok());
        CHECK_EQ(*sr, static_cast<int64_t>(2));
        CHECK(buf[0] == 'x' && buf[1] == 'y');
        CHECK(!p.slave_tty().take_eof());  // not EOF -- a real line was delivered
    }

    // Test 9: partial slave write when the master ring fills, then drain.
    {
        Pty  p;
        // Fill the 4096-byte master ring entirely from the slave side.
        char chunk[256];
        std::memset(chunk, 'Z', sizeof(chunk));
        size_t pushed = 0;
        while (pushed + sizeof(chunk) <= Pty::kMasterBufSize) {
            auto sw = p.slave_write(chunk, sizeof(chunk));
            CHECK(sw.ok());
            if (*sw == 0) {
                break;  // ring full
            }
            pushed += static_cast<size_t>(*sw);
        }
        CHECK(pushed == Pty::kMasterBufSize);  // exactly full
        auto full_check = p.slave_write(chunk, 1);
        CHECK(full_check.ok() && *full_check == 0);  // no space -> 0 accepted
        // Drain it all back; every byte is 'Z'.
        char   drain[256];
        size_t drained = 0;
        while (true) {
            auto mr = p.master_read(drain, sizeof(drain));
            CHECK(mr.ok());
            if (*mr == 0) {
                break;
            }
            for (int64_t i = 0; i < *mr; ++i) {
                CHECK(drain[i] == 'Z');
            }
            drained += static_cast<size_t>(*mr);
        }
        CHECK(drained == Pty::kMasterBufSize);
    }

    // Test 10: two independent pairs don't cross-talk.
    {
        Pty a;
        Pty b;
        CHECK(a.master_write("A\n", 2).ok());
        char buf[8];
        auto sr_b = b.slave_read(buf, sizeof(buf));
        CHECK(sr_b.ok() && *sr_b == 0);  // B sees nothing from A
        auto sr_a = a.slave_read(buf, sizeof(buf));
        CHECK(sr_a.ok() && *sr_a == 2 && buf[0] == 'A');
    }

    if (g_fail) {
        std::printf("pty tests FAILED\n");
        return 1;
    }
    std::printf("pty tests OK (10 cases)\n");
    return 0;
}
