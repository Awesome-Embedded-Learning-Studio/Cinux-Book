#include "kernel/syscall/sys_close.hpp"

#include "cinux/result.hpp"
#include "kernel/fs/file_table.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::syscall {

long long HandleClose(unsigned long long descriptor, [[maybe_unused]] unsigned long long second,
                      [[maybe_unused]] unsigned long long third) {
    if (descriptor < 3) {
        return 0;
    }
    if (descriptor >= fs::kFileTableMax) {
        return -kEbadf;
    }
    const base::Result<void> kFreed = CurrentTask()->files.free(descriptor);
    return kFreed.ok() ? 0 : -kEbadf;
}

}  // namespace cinux::syscall
