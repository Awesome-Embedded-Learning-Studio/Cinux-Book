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

bool InodeOps::is_page_cacheable() const {
    return false;
}

}  // namespace cinux::fs
