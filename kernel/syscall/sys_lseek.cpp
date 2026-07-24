/**
 * @file kernel/syscall/sys_lseek.cpp
 * @brief sys_lseek handler implementation (F10-M1 batch 4)
 *
 * Updates the File offset under its lock for SEEK_SET / SEEK_CUR / SEEK_END
 * and returns the resulting offset.  Legacy fds (stdin/stdout without a
 * VFS entry) are not seekable and return -ESPIPE.
 */

#include "kernel/syscall/sys_lseek.hpp"

#include <stdint.h>

#include "kernel/errno.hpp"
#include "kernel/fs/file.hpp"
#include "kernel/fs/vfs_mount.hpp"

namespace cinux::syscall {

namespace {

constexpr uint64_t kSeekSet = 0;  ///< offset from start of file
constexpr uint64_t kSeekCur = 1;  ///< offset relative to current position
constexpr uint64_t kSeekEnd = 2;  ///< offset relative to end of file

}  // anonymous namespace

int64_t sys_lseek(uint64_t fd, uint64_t offset, uint64_t whence, uint64_t, uint64_t, uint64_t) {
    cinux::fs::File* file = cinux::fs::current_fd_table().get(static_cast<int>(fd));
    if (file == nullptr || file->inode == nullptr) {
        return -cinux::kEbadf;
    }

    auto g = file->offset_lock_.guard();

    int64_t signed_offset = static_cast<int64_t>(offset);
    int64_t base          = 0;
    switch (whence) {
    case kSeekSet:
        base = 0;
        break;
    case kSeekCur:
        base = static_cast<int64_t>(file->offset);
        break;
    case kSeekEnd:
        base = static_cast<int64_t>(file->inode->size);
        break;
    default:
        return -cinux::kEinval;
    }

    int64_t new_offset = base + signed_offset;
    if (new_offset < 0) {
        return -cinux::kEinval;
    }

    file->offset = static_cast<uint64_t>(new_offset);
    return new_offset;
}

}  // namespace cinux::syscall
