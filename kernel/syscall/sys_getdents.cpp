#include "kernel/syscall/sys_getdents.hpp"

#include "cinux/memory.hpp"
#include "cinux/result.hpp"
#include "kernel/fs/file_table.hpp"
#include "kernel/fs/fs_config.hpp"
#include "kernel/fs/vfs.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::syscall {

long long HandleGetdents(unsigned long long descriptor, unsigned long long buffer,
                         [[maybe_unused]] unsigned long long third) {
    if (descriptor < 3) {
        return -kEbadf;
    }
    if (buffer >= kUserAddressLimit) {
        return -kEfault;
    }
    const base::Result<fs::FileSlot*> kSlot = LiveSlot(descriptor);
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
        reinterpret_cast<SyscallDirent*>(buffer);  // NOLINT(performance-no-int-to-ptr)
    base::CopyBytes(kOut->name, entry.name, fs::kFsNameMax);
    kOut->type = entry.type == fs::InodeType::kDirectory ? 1 : 0;
    return 1;
}

}  // namespace cinux::syscall
