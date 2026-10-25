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

#include "cinux/result.hpp"
#include "cinux/shared.hpp"
#include "kernel/fs/file_table.hpp"
#include "kernel/fs/open_file.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::syscall {

proc::Task* CurrentTask() {
    return proc::Scheduler::self().current();
}

base::Result<base::Shared<fs::OpenFile>> LiveSlot(unsigned long long descriptor) {
    if (descriptor >= fs::kFileTableMax) {
        return base::KernelError::kInvalidArgument;
    }
    return CurrentTask()->files->shared(descriptor);
}

}  // namespace cinux::syscall
