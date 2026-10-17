#include "kernel/syscall/sys_brk.hpp"

#include "cinux/result.hpp"
#include "kernel/mm/vma.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::syscall {

long long HandleBrk(const SyscallFrame& frame) {
    const proc::Task* const kSelf = CurrentTask();
    if (kSelf == nullptr || !kSelf->vmas.holds()) {
        return 0;
    }
    const base::Result<unsigned long> kMoved = kSelf->vmas->move_heap_end(frame.rdi);
    if (kMoved.ok()) {
        return static_cast<long long>(kMoved.value());
    }
    return static_cast<long long>(kSelf->vmas->heap_end());
}

}  // namespace cinux::syscall
