/**
 * @file    sys_common.cpp
 * @brief   The corridor every handler walks: errno, user paths, tasks.
 *
 * @author  Charliechen114514
 * @date    2026-10-11
 * @version 0.2
 * @since   0.1.0
 * @ingroup kernel_syscall
 * @copyright Copyright (c) 2026
 */


#include "cinux/result.hpp"
#include "cinux/shared.hpp"
#include "kernel/fs/file_table.hpp"
#include "kernel/fs/open_file.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::syscall {

unsigned long FileDescriptorLimit() {
    const auto kLimit = CurrentTask()->limits[7][0];
    return static_cast<unsigned long>(kLimit < fs::kFileTableMax ? kLimit : fs::kFileTableMax);
}

int ErrnoFrom(base::KernelError error) {
    switch (error) {
    case base::KernelError::kNotFound:
        return kEnoent;
    case base::KernelError::kTooManyOpenFiles:
        return kEmfile;
    case base::KernelError::kOutOfMemory:
        return kEnomem;
    case base::KernelError::kIOError:
        return kEio;
    case base::KernelError::kAlreadyExists:
        return kEexist;
    case base::KernelError::kNotADirectory:
        return kEnotdir;
    case base::KernelError::kIsADirectory:
        return kEisdir;
    case base::KernelError::kDirectoryNotEmpty:
        return kEnotempty;
    case base::KernelError::kWouldBlock:
        return kEagain;
    case base::KernelError::kInterrupted:
        return kEintr;
    case base::KernelError::kBrokenPipe:
        return kEpipe;
    default:
        return kEinval;
    }
}

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
