#include "kernel/syscall/sys_readlink.hpp"

#include "cinux/result.hpp"
#include "kernel/arch/x86_64/irq_guard.hpp"
#include "kernel/fs/file_world.hpp"
#include "kernel/fs/fs_config.hpp"
#include "kernel/fs/procfs/procfs.hpp"
#include "kernel/fs/vfs.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::syscall {

long long HandleReadlink(const SyscallFrame& frame) {
    if (frame.rsi == 0 || frame.rsi >= kUserAddressLimit || frame.rdx == 0) {
        return -kEfault;
    }
    if (!UserRangeWritable(frame.rsi, 1)) {
        return -kEfault;
    }
    char storage[fs::kFsPathMax] = {};
    if (const long long kBad = FoldUserPath(frame.rdi, storage, fs::kFsPathMax); kBad < 0) {
        return kBad;
    }
    const base::Result<fs::FsView> kView = fs::FileWorld::self().resolve(storage);
    if (!kView.ok()) {
        return -ErrnoFrom(kView.error());
    }
    const fs::FsView kFlat = kView.value();

    const arch::IrqGuard           kGuard;
    const base::Result<fs::FsNode> kNode = kFlat.ops->lookup(kFlat.self, kFlat.rest);
    if (!kNode.ok()) {
        return -ErrnoFrom(kNode.error());
    }
    if (kNode.value().ino != fs::kProcExeIno || kNode.value().type != fs::InodeType::kFile) {
        return -kEinval;
    }
    const proc::Task* const kSelf  = CurrentTask();
    unsigned long long      length = 0;
    while (length + 1 < sizeof(kSelf->exe) && kSelf->exe[length] != '\0') {
        ++length;
    }
    const unsigned long long kGive = length < frame.rdx ? length : frame.rdx;
    if (kGive != 0 && !UserRangeWritable(frame.rsi, kGive)) {
        return -kEfault;
    }
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto* const kOut = reinterpret_cast<char*>(frame.rsi);
    for (unsigned long long index = 0; index < kGive; ++index) {
        kOut[index] = kSelf->exe[index];
    }
    return static_cast<long long>(kGive);
}

}  // namespace cinux::syscall
