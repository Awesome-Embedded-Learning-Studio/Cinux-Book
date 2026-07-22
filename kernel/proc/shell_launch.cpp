/**
 * @file kernel/proc/shell_launch.cpp
 * @brief Non-GUI userspace launch (cinux::proc::launch_userspace, !CINUX_GUI build)
 *
 * CODING-TASTE §14: this is the non-GUI counterpart of
 * kernel/gui/desktop_launch.cpp.  It forks and execves /bin/sh directly, using
 * legacy sys_read (keyboard polling) for stdin and legacy sys_write (kprintf
 * serial+console) for stdout.  CMake links exactly one of the two
 * launch_userspace() implementations (this file is added only under
 * if(NOT CINUX_GUI) in proc/CMakeLists.txt).
 */

#include <stdint.h>

#include "kernel/lib/kprintf.hpp"
#include "kernel/mm/address_space.hpp"
#include "kernel/proc/process.hpp"  // fork()
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/user_launch.hpp"
#include "kernel/proc/userspace.hpp"

namespace cinux::proc {

void launch_userspace() {
    cinux::lib::kprintf("[INIT] Launching shell (non-GUI mode)...\n");
    int child_pid = fork(g_pid_alloc);
    if (child_pid == 0) {
        __asm__ volatile("cli");

        auto* task       = Scheduler::current();
        task->addr_space = new cinux::mm::AddressSpace();

        const char* path   = "/bin/sh";
        const char* argv[] = {path, nullptr};
        const char* envp[] = {nullptr};

        launch_user_program(path, argv, envp);
    } else if (child_pid > 0) {
        cinux::lib::kprintf("[INIT] Shell spawned pid=%d\n", child_pid);
    } else {
        cinux::lib::kprintf("[INIT] fork() failed: %d\n", child_pid);
    }
}

void handoff_framebuffer_to_gui(cinux::drivers::Framebuffer& /*fb*/,
                                cinux::drivers::PSFFont& /*font*/,
                                cinux::drivers::Console& /*console*/) {
    // GUI compiled out -- nothing to hand off.  §14 stub (paired with
    // desktop_launch.cpp's real impl; CMake links one).
}

}  // namespace cinux::proc
