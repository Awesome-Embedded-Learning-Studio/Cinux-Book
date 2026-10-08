#include "cinux/result.hpp"
#include "kernel/fs/file_table.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::syscall {

int ErrnoFrom(base::KernelError error) {
    switch (error) {
    case base::KernelError::kNotFound:
        return kEnoent;
    case base::KernelError::kOutOfMemory:
        return kEnomem;
    case base::KernelError::kAlreadyExists:
        return kEexist;
    case base::KernelError::kNotADirectory:
        return kEnotdir;
    case base::KernelError::kIsADirectory:
        return kEisdir;
    case base::KernelError::kDirectoryNotEmpty:
        return kEnotempty;
    default:
        return kEinval;
    }
}

long long CopyUserPath(unsigned long long source, char* out, unsigned long cap) {
    if (source >= kUserAddressLimit) {
        return -kEfault;
    }
    auto const* text = reinterpret_cast<const char*>(source);  // NOLINT(performance-no-int-to-ptr)
    for (unsigned long index = 0; index < cap; ++index) {
        out[index] = text[index];
        if (out[index] == '\0') {
            return index == 0 ? -kEinval : static_cast<long long>(index);
        }
    }
    return -kEinval;
}

proc::Task* CurrentTask() {
    return proc::Scheduler::self().current();
}

base::Result<fs::FileSlot*> LiveSlot(unsigned long long descriptor) {
    if (descriptor >= fs::kFileTableMax) {
        return base::KernelError::kInvalidArgument;
    }
    return CurrentTask()->files.get(descriptor);
}


}  // namespace cinux::syscall
