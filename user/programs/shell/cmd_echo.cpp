/**
 * @file user/programs/shell/cmd_echo.cpp
 * @brief Built-in 'echo' command implementation
 *
 * Prints all arguments separated by single spaces, followed by a newline.
 */

#include "shell.hpp"
#include "libc/string.hpp"
#include "libc/syscall.h"

using cinux::user::strlen;

namespace {

void write_str(const char* s) {
    sys_write(1, s, strlen(s));
}

}  // anonymous namespace

void cmd_echo(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (i > 1) {
            sys_write(1, " ", 1);
        }
        write_str(argv[i]);
    }
    sys_write(1, "\n", 1);
}
