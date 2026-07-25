/**
 * @file user/programs/shell/cmd_mkfifo.cpp
 * @brief Built-in 'mkfifo' command implementation (F8-M2)
 *
 * Creates a named FIFO (named pipe) at the given path via sys_mknod with
 * S_IFIFO.  Usage: mkfifo <path>
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

void cmd_mkfifo(int argc, char** argv) {
    if (argc < 2) {
        write_str("mkfifo: missing file operand\n");
        return;
    }

    const char* path   = argv[1];
    int64_t     result = sys_mknod(path, S_IFIFO | 0666, 0);

    if (result < 0) {
        write_str("mkfifo: cannot create '");
        write_str(path);
        write_str("'\n");
    }
}
