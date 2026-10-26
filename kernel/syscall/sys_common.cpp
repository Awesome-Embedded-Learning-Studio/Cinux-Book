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

#include <algorithm>

#include "cinux/own.hpp"
#include "cinux/path.hpp"
#include "cinux/result.hpp"
#include "cinux/shared.hpp"
#include "kernel/arch/x86_64/page.hpp"
#include "kernel/fs/file_table.hpp"
#include "kernel/fs/fs_config.hpp"
#include "kernel/fs/open_file.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"
#include "kernel/syscall/uaccess.hpp"

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

long long CopyUserPath(unsigned long long source, char* out, unsigned long cap) {
    if (source == 0 || source >= kUserAddressLimit) {
        return -kEfault;
    }
    constexpr unsigned long kChunkBytes = 64;
    unsigned long           done        = 0;
    while (done < cap) {
        const unsigned long kInPage =
            cinux::arch::page::kSize - ((source + done) % cinux::arch::page::kSize);
        const unsigned long kChunk = std::min({kChunkBytes, cap - done, kInPage});
        if (!uaccess::CopyFromUser(out + done, source + done, kChunk)) {
            return -kEfault;
        }
        for (unsigned long index = done; index < done + kChunk; ++index) {
            if (out[index] == '\0') {
                return index == 0 ? -kEinval : static_cast<long long>(index);
            }
        }
        done += kChunk;
    }
    return -kEinval;
}

long long FoldUserPath(unsigned long long source, char* out, unsigned long cap) {
    const base::Own<char[]> kRawPath(new char[fs::kUserPathMax]);
    if (!kRawPath.holds()) {
        return -kEnomem;
    }
    const long long kRaw = CopyUserPath(source, kRawPath.get(), fs::kUserPathMax);
    if (kRaw < 0) {
        return kRaw;
    }
    const char* const  kBase = kRawPath[0] == '/' ? "/" : CurrentTask()->cwd;
    const base::Folded kFold = base::FoldOntoPath(kBase, kRawPath.get(), out, cap);
    if (!kFold.ok) {
        return -kEnametoolong;
    }
    return static_cast<long long>(kFold.length);
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
