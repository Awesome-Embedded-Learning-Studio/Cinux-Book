/**
 * @file kernel/fs/ext2/ext2_metadata.cpp
 * @brief Ext2 inode metadata operations and symlink target reads
 */

#include <stdint.h>

#include "ext2.hpp"

namespace cinux::fs {

bool Ext2::chmod(uint32_t ino, uint32_t mode) {
    Ext2Inode d;
    if (!read_disk_inode(ino, d)) {
        return false;
    }
    // Keep the file-type bits (high nibble), replace only the low 12 perms.
    d.i_mode = static_cast<uint16_t>((d.i_mode & EXT2_S_IFMT) | (mode & 0x0FFF));
    if (!write_disk_inode(ino, d)) {
        return false;
    }
    invalidate_cached_inode(ino);  // stat() reads the cache; drop the stale copy.
    return true;
}

bool Ext2::chown(uint32_t ino, uint32_t uid, uint32_t gid) {
    Ext2Inode d;
    if (!read_disk_inode(ino, d)) {
        return false;
    }
    // rev-0 inode stores only the low 16 bits of uid/gid; we drop the high
    // 16 bits on purpose (hobby-OS simplification, matches on-disk layout).
    if (uid != 0xFFFFFFFFu) {
        d.i_uid = static_cast<uint16_t>(uid);
    }
    if (gid != 0xFFFFFFFFu) {
        d.i_gid = static_cast<uint16_t>(gid);
    }
    if (!write_disk_inode(ino, d)) {
        return false;
    }
    invalidate_cached_inode(ino);  // stat() reads the cache; drop the stale copy.
    return true;
}

bool Ext2::utimensat(uint32_t ino, uint64_t atime_sec, uint32_t /*atime_nsec*/, uint64_t mtime_sec,
                     uint32_t /*mtime_nsec*/) {
    Ext2Inode d;
    if (!read_disk_inode(ino, d)) {
        return false;
    }
    // rev-0 inode only stores seconds; nsec is intentionally dropped.
    d.i_atime = static_cast<uint32_t>(atime_sec);
    d.i_mtime = static_cast<uint32_t>(mtime_sec);
    if (!write_disk_inode(ino, d)) {
        return false;
    }
    invalidate_cached_inode(ino);  // stat() reads the cache; drop the stale copy.
    return true;
}

void Ext2::invalidate_cached_inode(uint32_t ino) {
    for (uint32_t i = 0; i < EXT2_INODE_CACHE_SIZE; ++i) {
        if (inode_cache_[i].in_use && inode_cache_[i].ino == ino) {
            inode_cache_[i].in_use = false;
            return;
        }
    }
}

int64_t Ext2::readlink(uint32_t ino, char* buf, uint64_t buf_size) {
    if (buf == nullptr || buf_size == 0) {
        return -1;
    }
    Ext2Inode d;
    if (!read_disk_inode(ino, d)) {
        return -1;
    }
    // Symlink target lives in the first data block (i_block[0]), not inlined.
    if (d.i_block[0] == 0) {
        return -1;
    }
    if (!read_block(d.i_block[0])) {
        return -1;
    }
    // Copy up to min(i_size, buf_size) bytes; no NUL terminator (Linux readlink).
    uint64_t n =
        (static_cast<uint64_t>(d.i_size) < buf_size) ? static_cast<uint64_t>(d.i_size) : buf_size;
    const uint8_t* src = block_buf();
    for (uint64_t i = 0; i < n; ++i) {
        buf[i] = static_cast<char>(src[i]);
    }
    return static_cast<int64_t>(n);
}

}  // namespace cinux::fs
