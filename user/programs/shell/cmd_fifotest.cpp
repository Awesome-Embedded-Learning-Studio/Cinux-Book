/**
 * @file user/programs/shell/cmd_fifotest.cpp
 * @brief Built-in 'fifotest' command: end-to-end FIFO smoke (F8-M2)
 *
 * Exercises the named-FIFO path from user space, all within the shell process:
 *   mkfifo(/dev/ft) -> open write end -> open read end -> write -> read -> print
 *
 * Both ends are held open at once (no close between write and read), so the
 * shared pipe has a live reader; the single blocking read returns the buffered
 * bytes immediately and never blocks.  This proves the whole chain works end to
 * end: sys_mknod registers the name, DevFS resolves /dev/ft to the FIFO inode,
 * the cloning open() builds the shared pipe, and the read/write ends round-trip
 * data.  Usage: fifotest
 */

#include "libc/string.hpp"
#include "libc/syscall.h"
#include "shell.hpp"

using cinux::user::strlen;

namespace {

void write_str(const char* s) {
    sys_write(1, s, strlen(s));
}

}  // anonymous namespace

void cmd_fifotest(int /*argc*/, char** /*argv*/) {
    const char* path = "/dev/ft";

    // Register the FIFO.  Ignore "already exists" so the smoke is re-runnable:
    // DevFS has no unlink, so a name registered by a prior run stays, but the
    // shared pipe is drained by each read, so stale state is harmless.
    sys_mknod(path, S_IFIFO | 0666, 0);

    // Open both ends.  Each open() goes through sys_open -> do_open_kernel ->
    // InodeOps::open cloning: the first builds the shared pipe, the second
    // reuses it; direction (O_WRONLY/O_RDONLY) selects the write/read end.
    int64_t wfd = sys_open(path, O_WRONLY);
    int64_t rfd = sys_open(path, O_RDONLY);
    if (wfd < 0 || rfd < 0) {
        write_str("fifotest: open failed\n");
        if (wfd >= 0) {
            sys_close(static_cast<int>(wfd));
        }
        if (rfd >= 0) {
            sys_close(static_cast<int>(rfd));
        }
        return;
    }

    // Write a message through the write end, then read it back through the read
    // end of the same shared pipe.
    const char msg[] = "hello from fifo!\n";
    sys_write(static_cast<int>(wfd), msg, strlen(msg));

    char    buf[64];
    int64_t n = sys_read(static_cast<int>(rfd), buf, sizeof(buf));
    if (n > 0) {
        sys_write(1, buf, static_cast<size_t>(n));  // echoes "hello from fifo!"
    }

    write_str(n > 0 ? "fifotest: OK\n" : "fifotest: read failed\n");

    sys_close(static_cast<int>(wfd));
    sys_close(static_cast<int>(rfd));
}
