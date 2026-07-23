/**
 * @file kernel/proc/shell_launch.cpp
 * @brief Non-GUI userspace launch (cinux::proc::launch_userspace, !CINUX_GUI build)
 *
 * CODING-TASTE §14: this is the non-GUI counterpart of
 * kernel/gui/desktop_launch.cpp.  B3b (GCC self-host): the init kthread --
 * already PID1 (kernel_init_thread allocs it) -- execves /sbin/init directly
 * (no fork), so busybox init runs as PID1 and reaps orphaned children.  busybox
 * init then forks / respawns /bin/sh per /etc/inittab; the spawned ash reads
 * stdin / writes stdout through /dev/console (devfs ConsoleDevOps proxies the
 * console TTY).  CMake links exactly one of the two launch_userspace()
 * implementations (this file is added only under if(NOT CINUX_GUI) in
 * proc/CMakeLists.txt).
 */

#include <stdint.h>

#include "kernel/lib/kprintf.hpp"
#include "kernel/mm/address_space.hpp"
#include "kernel/proc/process.hpp"  // Task (Scheduler::current()->addr_space)
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/user_launch.hpp"
#include "kernel/proc/userspace.hpp"

namespace cinux::proc {

void launch_userspace() {
    // B3b (GCC self-host): the init kthread itself execves /sbin/init -- it
    // already took PID1 in kernel_init_thread -- instead of forking a shell
    // child.  busybox init (argv[0] basename "init") then forks / respawns
    // /bin/sh per /etc/inittab, and as PID1 it reaps orphaned children (the
    // hard prerequisite for B4 GCC's cc1/as/ld fork chains).  execve replaces
    // the image and does not return.
    cinux::lib::kprintf("[INIT] Executing /sbin/init as PID1 (busybox init)...\n");

    auto* self = Scheduler::current();
    // execve requires a non-null address space (kernel threads start without
    // one): install a fresh one.  execve clears any mappings first, so the
    // kernel thread never leaks user mappings into init.
    self->addr_space = new cinux::mm::AddressSpace();

    const char* path   = "/sbin/init";
    const char* argv[] = {"init", nullptr};  // basename "init" -> busybox init applet
    const char* envp[] = {nullptr};
    launch_user_program(path, argv, envp);  // execve + jump_to_usermode; no return
}

void handoff_framebuffer_to_gui(cinux::drivers::Framebuffer& /*fb*/,
                                cinux::drivers::PSFFont& /*font*/,
                                cinux::drivers::Console& /*console*/) {
    // GUI compiled out -- nothing to hand off.  §14 stub (paired with
    // desktop_launch.cpp's real impl; CMake links one).
}

}  // namespace cinux::proc
