/**
 * @file    syscall_host.cpp
 * @brief   Host-world second implementations of the syscall seams.
 *
 * The declarations live once in syscall_internal.hpp, the kernel world
 * fills them in sys_common.cpp, and linking picks the world.
 * assert_host.cpp and irq_guard_host.cpp set the same pattern one
 * theme over.
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
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::syscall {

proc::Task* CurrentTask() {
    return proc::Scheduler::self().current();
}

bool UserRangeReadable(Word start, Word bytes) {
    return start < kUserAddressLimit && bytes <= kUserAddressLimit - start;
}

bool UserRangeWritable(Word start, Word bytes) {
    return UserRangeReadable(start, bytes);
}

}  // namespace cinux::syscall
