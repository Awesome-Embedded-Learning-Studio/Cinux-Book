/**
 * @file host/posix_spawn.cpp
 * @brief Linux host: fork a child under a PTY (P4-d)
 *
 * HOST layer. Uses forkpty (libutil) so the child runs in a controlling tty --
 * shells get line editing and curses programs work.
 */

#include "posix_spawn.hpp"

#include <pty.h>     // forkpty
#include <unistd.h>  // execvp, _exit

namespace cinux::gui {

int linux_spawn(void* /*ctx*/, const char* path, char* const argv[], int* stdin_fd,
                int* stdout_fd) {
    int   master = -1;
    pid_t pid    = forkpty(&master, nullptr, nullptr, nullptr);
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        // child: argv[0] is the shell; exec replaces us
        execvp(path, argv);
        _exit(127);  // unreachable on success
    }
    // parent: master is bidirectional (write -> child stdin, read <- child stdout)
    if (stdin_fd != nullptr) {
        *stdin_fd = master;
    }
    if (stdout_fd != nullptr) {
        *stdout_fd = master;
    }
    return static_cast<int>(pid);
}

}  // namespace cinux::gui
