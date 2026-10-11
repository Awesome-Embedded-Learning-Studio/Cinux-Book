#include "kernel/syscall/sys_getcwd.hpp"

#include "cinux/math.hpp"
#include "cinux/memory.hpp"
#include "kernel/fs/fs_config.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::syscall {

long long HandleGetcwd(unsigned long long out, unsigned long long size,
                       [[maybe_unused]] unsigned long long third) {
    if (out >= kUserAddressLimit) {
        return -kEfault;
    }
    const unsigned long kRoom = base::math::Min(size, fs::kFsPathMax);
    const char* const   kCwd  = CurrentTask()->cwd;
    unsigned long       used  = 0;
    while (used + 1 < kRoom && kCwd[used] != '\0') {
        ++used;
    }
    if (kCwd[used] != '\0') {
        return -kErange;
    }
    auto* const kSink = reinterpret_cast<char*>(out);  // NOLINT(performance-no-int-to-ptr)
    base::CopyBytes(kSink, kCwd, used + 1UL);
    return static_cast<long long>(used);
}

}  // namespace cinux::syscall
