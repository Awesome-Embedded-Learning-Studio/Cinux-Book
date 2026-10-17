#include "kernel/syscall/sys_close.hpp"

#include "cinux/result.hpp"
#include "kernel/fs/file_table.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::syscall {

long long HandleClose(const SyscallFrame& frame) {
    if (frame.rdi < 3) {
        return 0;
    }
    if (frame.rdi >= fs::kFileTableMax) {
        return -kEbadf;
    }
    const base::Result<void> kFreed = CurrentTask()->files->free(frame.rdi);
    return kFreed.ok() ? 0 : -kEbadf;
}

}  // namespace cinux::syscall
