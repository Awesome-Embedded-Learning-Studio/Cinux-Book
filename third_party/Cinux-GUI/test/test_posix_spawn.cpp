/**
 * @file test/test_posix_spawn.cpp
 * @brief P4-d posix_spawn -- fork a shell under a PTY, talk to it, read output
 *
 * Spawns `/bin/sh -c "echo hi"` via linux_spawn (forkpty + exec), then reads
 * the PTY master until EOF and asserts the echoed "hi" came back. Exercises the
 * real fork/exec/PTY path that terminal-host relies on.
 *
 * Standalone ctest (host layer; needs libutil for forkpty). No SDL.
 */
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cassert>
#include <cstdio>

#include "posix_spawn.hpp"

using namespace cinux::gui;

int main() {
    const char* path = "/bin/sh";
    char* argv[] = {const_cast<char*>("sh"), const_cast<char*>("-c"), const_cast<char*>("echo hi"),
                    nullptr};

    int in_fd  = -1;
    int out_fd = -1;
    int pid    = linux_spawn(nullptr, path, argv, &in_fd, &out_fd);
    assert(pid > 0);
    assert(in_fd >= 0 && out_fd >= 0);
    assert(in_fd == out_fd);  // both are the PTY master

    // non-blocking read; child echoes then exits -> read returns 0 (EOF)
    fcntl(out_fd, F_SETFL, O_NONBLOCK);

    char     buf[256] = {};
    uint32_t total    = 0;
    for (int i = 0; i < 200; ++i) {  // up to ~200 ms
        if (total >= sizeof(buf) - 1) {
            break;
        }
        ssize_t n = read(out_fd, buf + total, sizeof(buf) - 1 - total);
        if (n > 0) {
            total += static_cast<uint32_t>(n);
            buf[total] = '\0';
        } else if (n == 0) {
            break;  // EOF (child exited)
        }
        usleep(1000);  // EAGAIN or not-yet-ready -> wait briefly
    }
    assert(total > 0);
    assert(strstr(buf, "hi") != nullptr);

    int status = 0;
    waitpid(pid, &status, 0);
    close(in_fd);

    std::printf("test_posix_spawn: OK (read %u bytes: %s)\n", total, buf);
    return 0;
}
