#include "kernel/syscall/sys_access.hpp"

#include <cstdint>

#include "cinux/result.hpp"
#include "kernel/arch/x86_64/irq_guard.hpp"
#include "kernel/fs/file_world.hpp"
#include "kernel/fs/fs_config.hpp"
#include "kernel/fs/vfs.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::syscall {

long long HandleAccess(const SyscallFrame& frame) {
    char storage[fs::kFsPathMax] = {};
    if (const long long kBad = FoldUserPath(frame.rdi, storage, fs::kFsPathMax); kBad < 0) {
        return kBad;
    }
    const base::Result<fs::FsView> kView = fs::FileWorld::self().resolve(storage);
    if (!kView.ok()) {
        return -ErrnoFrom(kView.error());
    }
    const arch::IrqGuard kGuard;
    const fs::FsView&    flat  = kView.value();
    const auto           kNode = flat.ops->lookup(flat.self, flat.rest);
    return kNode.ok() ? 0 : -ErrnoFrom(kNode.error());
}

long long HandleFaccessat(const SyscallFrame& frame) {
    if (static_cast<std::int32_t>(frame.rdi) != kAtFdcwd) {
        return -kEnotdir;
    }
    if (frame.rax == static_cast<unsigned long long>(SyscallNr::kFaccessat2) &&
        (frame.r10 & ~0x200ULL) != 0) {
        return -kEinval;
    }
    SyscallFrame flat{};
    flat.rdi = frame.rsi;
    flat.rsi = frame.rdx;
    return HandleAccess(flat);
}

}  // namespace cinux::syscall
