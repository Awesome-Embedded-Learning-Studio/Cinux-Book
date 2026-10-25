#include "kernel/syscall/sys_mount.hpp"

#include "cinux/result.hpp"
#include "kernel/fs/file_world.hpp"
#include "kernel/fs/fs_config.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"
#include "kernel/syscall/uaccess.hpp"

namespace cinux::syscall {

namespace {

constexpr unsigned long kTypeWordMax = 8;

/// Copies one short NUL-terminated word from ring 3; false when the
/// word walks out of the user half or fills the room without ending.
bool fold_user_word(unsigned long address, char* out, unsigned long cap) {
    unsigned long done = 0;
    while (done < cap - 1) {
        if (address + done >= kUserAddressLimit ||
            !uaccess::CopyFromUser(out + done, address + done, 1)) {
            return false;
        }
        if (out[done] == '\0') {
            return true;
        }
        ++done;
    }
    return false;
}

}  // namespace

long long HandleMount(const SyscallFrame& frame) {
    if (CurrentTask()->euid != 0) {
        return -kEperm;
    }
    constexpr Word kSilent = 0x8000;
    if ((frame.r10 & ~kSilent) != 0) {
        return -kEopnotsupp;
    }
    char            target[fs::kFsPathMax] = {};
    const long long kTarget                = FoldUserPath(frame.rsi, target, sizeof(target));
    if (kTarget < 0) {
        return kTarget;
    }
    char type[kTypeWordMax] = {};
    if (!fold_user_word(frame.rdx, type, sizeof(type))) {
        return -kEfault;
    }
    const bool kIsTmpfs = type[0] == 't' && type[1] == 'm' && type[2] == 'p' && type[3] == 'f' &&
                          type[4] == 's' && type[5] == '\0';
    if (!kIsTmpfs) {
        return -kEnodev;
    }
    const base::Result<void> kSeated = fs::FileWorld::self().mount_tmpfs(target);
    if (!kSeated.ok()) {
        return -ErrnoFrom(kSeated.error());
    }
    return 0;
}

long long HandleUmount2(const SyscallFrame& frame) {
    if (CurrentTask()->euid != 0) {
        return -kEperm;
    }
    constexpr Word kDetach = 2;
    if ((frame.rsi & ~kDetach) != 0) {
        return -kEinval;
    }
    char            target[fs::kFsPathMax] = {};
    const long long kTarget                = FoldUserPath(frame.rdi, target, sizeof(target));
    if (kTarget < 0) {
        return kTarget;
    }
    const base::Result<void> kGone =
        fs::FileWorld::self().umount_tmpfs(target, frame.rsi == kDetach);
    if (!kGone.ok()) {
        if (kGone.error() == base::KernelError::kWouldBlock) {
            return -kEbusy;
        }
        return -ErrnoFrom(kGone.error());
    }
    return 0;
}

}  // namespace cinux::syscall
