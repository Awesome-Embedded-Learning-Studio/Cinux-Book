#include "kernel/syscall/sys_getdents.hpp"

#include "cinux/memory.hpp"
#include "cinux/result.hpp"
#include "kernel/fs/fs_config.hpp"
#include "kernel/fs/open_file.hpp"
#include "kernel/fs/vfs.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::syscall {

long long HandleGetdents(const SyscallFrame& frame) {
    if (frame.rdi < 3) {
        return -kEbadf;
    }
    if (frame.rsi >= kUserAddressLimit) {
        return -kEfault;
    }
    const base::Result<fs::OpenFile*> kSlot = LiveSlot(frame.rdi);
    if (!kSlot.ok()) {
        return -kEbadf;
    }
    auto* const kDir = kSlot.value();

    fs::FsDirent             entry{};
    const base::Result<bool> kOne =
        kDir->ops->read_dir(kDir->backend, kDir->node, kDir->offset, entry);
    if (!kOne.ok()) {
        return -ErrnoFrom(kOne.error());
    }
    if (!kOne.value()) {
        return 0;
    }
    kDir->offset++;

    using syscall::SyscallDirent;
    auto* const kOut =
        reinterpret_cast<SyscallDirent*>(frame.rsi);  // NOLINT(performance-no-int-to-ptr)
    base::CopyBytes(kOut->name, entry.name, fs::kFsNameMax);
    kOut->type = entry.type == fs::InodeType::kDirectory ? kDirentDirectory : kDirentFile;
    return 1;
}

}  // namespace cinux::syscall
