#include "kernel/syscall/sys_set_tid_address.hpp"

#include "kernel/proc/task.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::syscall {

long long HandleSetTidAddress(const SyscallFrame& frame) {
    if (frame.rdi != 0 && !UserRangeWritable(frame.rdi, sizeof(unsigned int))) {
        return -kEfault;
    }
    proc::Task* const kSelf = CurrentTask();
    kSelf->clear_child_tid  = frame.rdi;
    return static_cast<long long>(kSelf->pid);
}

}  // namespace cinux::syscall
