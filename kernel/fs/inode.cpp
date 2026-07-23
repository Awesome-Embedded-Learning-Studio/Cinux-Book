/**
 * @file kernel/fs/inode.cpp
 * @brief Default InodeOps method implementations
 *
 * Unsupported operations return -1 or nullptr by default.
 */

#include "inode.hpp"

namespace cinux::fs {

cinux::lib::ErrorOr<int64_t> InodeOps::read(const Inode*, uint64_t, void*, uint64_t) {
    return cinux::lib::Error::NotImplemented;
}

cinux::lib::ErrorOr<int64_t> InodeOps::write(Inode*, uint64_t, const void*, uint64_t) {
    return cinux::lib::Error::NotImplemented;
}

cinux::lib::ErrorOr<int64_t> InodeOps::readdir(const Inode*, uint64_t, char*, uint64_t) {
    return cinux::lib::Error::NotImplemented;
}

cinux::lib::ErrorOr<Inode*> InodeOps::create(Inode*, const char*, uint32_t) {
    return cinux::lib::Error::NotImplemented;
}

cinux::lib::ErrorOr<Inode*> InodeOps::mkdir(Inode*, const char*, uint32_t) {
    return cinux::lib::Error::NotImplemented;
}

cinux::lib::ErrorOr<void> InodeOps::unlink(Inode*, const char*, uint32_t) {
    return cinux::lib::Error::NotImplemented;
}

cinux::lib::ErrorOr<void> InodeOps::stat(const Inode*, struct stat*) {
    return cinux::lib::Error::NotImplemented;
}

cinux::lib::ErrorOr<int64_t> InodeOps::ioctl(const Inode*, uint32_t, uint64_t) {
    // "This inode type does not implement ioctls."  sys_ioctl translates this
    // into -ENOTTY for the caller (the Linux convention for an ioctl an inode
    // does not handle), so the default is observationally "not a tty ioctl".
    return cinux::lib::Error::NotImplemented;
}

cinux::lib::ErrorOr<Inode*> InodeOps::open(Inode* inode, uint64_t /*flags*/) {
    // Bind the fd to the inode lookup resolved -- no per-open clone.  A cloning
    // device (/dev/ptmx, a FIFO) overrides this to hand back a fresh resource,
    // honouring @p flags (direction / O_NONBLOCK).
    return inode;
}

// F-ECO batch 2: default attribute + dirent ops. Backends that do not support
// them inherit NotImplemented, which syscalls translate to -ENOSYS.
cinux::lib::ErrorOr<void> InodeOps::chmod(Inode*, uint32_t) {
    return cinux::lib::Error::NotImplemented;
}

cinux::lib::ErrorOr<void> InodeOps::chown(Inode*, uint32_t, uint32_t) {
    return cinux::lib::Error::NotImplemented;
}

cinux::lib::ErrorOr<void> InodeOps::utimensat(Inode*, uint64_t, uint32_t, uint64_t, uint32_t) {
    return cinux::lib::Error::NotImplemented;
}

cinux::lib::ErrorOr<int64_t> InodeOps::readlink(const Inode*, char*, uint64_t) {
    return cinux::lib::Error::NotImplemented;
}

cinux::lib::ErrorOr<void> InodeOps::symlink(Inode*, const char*, uint32_t, const char*) {
    return cinux::lib::Error::NotImplemented;
}

cinux::lib::ErrorOr<void> InodeOps::link(Inode*, const char*, uint32_t, const Inode*) {
    return cinux::lib::Error::NotImplemented;
}

cinux::lib::ErrorOr<void> InodeOps::rename(Inode*, const char*, uint32_t, Inode*, const char*,
                                           uint32_t) {
    return cinux::lib::Error::NotImplemented;
}

bool InodeOps::is_page_cacheable() const {
    return false;
}

}  // namespace cinux::fs
