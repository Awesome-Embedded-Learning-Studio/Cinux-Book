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

#include "cinux/path.hpp"
#include "cinux/result.hpp"
#include "kernel/fs/file_table.hpp"
#include "kernel/fs/fs_config.hpp"
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

long long FoldUserPath(unsigned long long source, char* out, unsigned long cap) {
    char            raw[fs::kFsPathMax] = {};
    const long long kRaw                = CopyUserPath(source, raw, sizeof(raw));
    if (kRaw < 0) {
        return kRaw;
    }
    const char* const  kBase = raw[0] == '/' ? "/" : CurrentTask()->cwd;
    const base::Folded kFold = base::FoldOntoPath(kBase, raw, out, cap);
    if (!kFold.ok) {
        return -kEnametoolong;
    }
    return static_cast<long long>(kFold.length);
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
