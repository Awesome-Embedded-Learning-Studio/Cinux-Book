#include "kernel/syscall/sys_getcwd.hpp"

#include "cinux/math.hpp"
#include "kernel/fs/fs_config.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"
#include "kernel/syscall/uaccess.hpp"

namespace cinux::syscall {

long long HandleGetcwd(const SyscallFrame& frame) {
    if (frame.rdi == 0) {
        return -kEfault;
    }
    const unsigned long kRoom = base::math::Min(frame.rsi, fs::kFsPathMax);
    const char* const   kCwd  = CurrentTask()->cwd;
    unsigned long       used  = 0;
    while (used + 1 < kRoom && kCwd[used] != '\0') {
        ++used;
    }
    if (kCwd[used] != '\0') {
        return -kErange;
    }
    if (!uaccess::CopyToUser(frame.rdi, kCwd, used + 1UL)) {
        return -kEfault;
    }
    return static_cast<long long>(used);
}

}  // namespace cinux::syscall
