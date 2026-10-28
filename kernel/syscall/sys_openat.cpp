#include "kernel/syscall/sys_openat.hpp"

#include <cstdint>

#include "kernel/fs/fs_config.hpp"
#include "kernel/fs/vfs.hpp"
#include "kernel/syscall/sys_open.hpp"
#include "kernel/syscall/sys_stat.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::syscall {
long long HandleOpenat(const SyscallFrame& frame) {
    char raw[fs::kFsPathMax]{};
    if (const auto kBad = CopyUserPath(frame.rsi, raw, sizeof(raw)); kBad < 0) {
        return kBad;
    }
    if (raw[0] != '/' && static_cast<std::int32_t>(frame.rdi) != kAtFdcwd) {
        const auto kDir = LiveSlot(frame.rdi);
        if (!kDir.ok()) {
            return -kEbadf;
        }
        if (kDir.value()->node.type != fs::InodeType::kDirectory) {
            return -kEnotdir;
        }
        if (kDir.value()->ops->lookup_at == nullptr) {
            return -kEopnotsupp;
        }
        return OpenPathAt(frame.rdi, raw, frame.rdx, frame.r10);
    }
    char path[fs::kFsPathMax]{};
    if (const auto kBad = FoldUserPathAt(frame.rdi, frame.rsi, path); kBad < 0) {
        return kBad;
    }
    return OpenPath(path, frame.rdx, frame.r10);
}

long long HandleNewfstatat(const SyscallFrame& frame) {
    char raw[fs::kFsPathMax]{};
    if (const auto kBad = CopyUserPath(frame.rsi, raw, sizeof(raw)); kBad < 0) {
        return kBad;
    }
    if (raw[0] != '/' && static_cast<std::int32_t>(frame.rdi) != kAtFdcwd) {
        const auto kDir = LiveSlot(frame.rdi);
        if (!kDir.ok()) {
            return -kEbadf;
        }
        if (kDir.value()->node.type != fs::InodeType::kDirectory) {
            return -kEnotdir;
        }
        if (kDir.value()->ops->lookup_at == nullptr) {
            return -kEopnotsupp;
        }
        return StatPathAt(frame.rdi, raw, frame.rdx);
    }
    char path[fs::kFsPathMax]{};
    if (const auto kBad = FoldUserPathAt(frame.rdi, frame.rsi, path); kBad < 0) {
        return kBad;
    }
    return StatPath(path, frame.rdx);
}
}
