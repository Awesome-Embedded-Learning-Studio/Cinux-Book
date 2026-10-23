/**
 * @file    syscall_host.cpp
 * @brief   Host-world second implementations of the syscall seams.
 *
 * The declarations live once in syscall_internal.hpp, the kernel world
 * fills them in sys_common.cpp, and linking picks the world.
 * assert_host.cpp and irq_guard_host.cpp set the same pattern one
 * theme over. The uaccess door needs no hosted twin: its body runs on
 * the host as-is, its arm bit stays off and the window never opens.
 *
 * @author  Charliechen114514
 * @date    2026-10-18
 * @version 0.3.0
 * @since   0.3.0
 * @ingroup test_framework
 * @copyright Copyright (c) 2026
 */

#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::syscall {

proc::Task* CurrentTask() {
    return proc::Scheduler::self().current();
}

}  // namespace cinux::syscall
