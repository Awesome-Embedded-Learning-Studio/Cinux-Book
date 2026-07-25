/**
 * @file kernel/syscall/sys_fcntl.cpp
 * @brief sys_fcntl handler (F-ECO batch 4)
 *
 * See sys_fcntl.hpp.  Resolves the File* once, then dispatches on cmd.  The
 * F_SETFD write to File::cloexec is racing only if two threads fcntl the same fd
 * concurrently -- rare at hobby scale (a per-fd flag), so it is left un-locked.
 *
 * Namespace: cinux::syscall
 */

#include "kernel/syscall/sys_fcntl.hpp"

#include <stdint.h>

#include "kernel/errno.hpp"
#include "kernel/fs/file.hpp"
#include "kernel/fs/vfs_mount.hpp"  // current_fd_table()

namespace cinux::syscall {

namespace {

// Linux x86_64 fcntl() cmd values.
constexpr uint64_t kFDupfd    = 0;
constexpr uint64_t kFGetfd    = 1;
constexpr uint64_t kFSetfd    = 2;
constexpr uint64_t kFGetfl    = 3;
constexpr uint64_t kFdCloexec = 1;  ///< FD_CLOEXEC flag bit (F_GETFD/F_SETFD)

/// Translate the kernel OpenFlags access mode into the Linux O_* bits returned
/// by F_GETFL.  O_NONBLOCK (and other status flags) are not tracked yet -> 0.
int64_t openflags_to_linux(cinux::fs::OpenFlags fl) {
    switch (fl) {
    case cinux::fs::OpenFlags::WRONLY:
        return 1;  // O_WRONLY
    case cinux::fs::OpenFlags::RDWR:
        return 2;  // O_RDWR
    default:
        return 0;  // O_RDONLY (RDONLY)
    }
}

}  // anonymous namespace

int64_t sys_fcntl(uint64_t fd, uint64_t cmd, uint64_t arg, uint64_t, uint64_t, uint64_t) {
    cinux::fs::FDTable& tbl = cinux::fs::current_fd_table();
    cinux::fs::File*    f   = tbl.get(static_cast<int>(fd));
    if (f == nullptr) {
        return -cinux::kEbadf;
    }
    switch (cmd) {
    case kFDupfd: {
        int nfd = tbl.dup(static_cast<int>(fd), static_cast<int>(arg));
        return nfd >= 0 ? static_cast<int64_t>(nfd) : -cinux::kEmfile;
    }
    case kFGetfd:
        return f->cloexec ? static_cast<int64_t>(kFdCloexec) : 0;
    case kFSetfd:
        f->cloexec = (arg & kFdCloexec) != 0;
        return 0;
    case kFGetfl:
        return openflags_to_linux(f->flags);
    default:
        return -cinux::kEinval;  // F_SETFL / unknown -- not supported yet
    }
}

}  // namespace cinux::syscall
