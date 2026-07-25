/**
 * @file kernel/fs/ext2/ext2_metadata.cpp
 * @brief Ext2 inode metadata operations and symlink target reads
 */

#include <stdint.h>

#include "ext2.hpp"
#include "ext2_types.hpp"  // EXT2_S_IF* mode bits (switch labels in populate_vfs_inode)

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
    // The on-disk inode for @p ino changed out-of-band (chmod / chown /
    // utimensat wrote a fresh disk_inode; or unlink freed it).  Drop the stale
    // cached copy.  If no one is referencing it (refcount == 0) we can free the
    // object now -- the next lookup re-reads from disk.  If a fd / VMA still
    // holds a reference (refcount > 0) we must keep the object (address-stability
    // invariant), so mark it stale: the next get_cached_inode() hit refreshes
    // the disk copy in place at the SAME address.
    const uint32_t bucket = ino % EXT2_INODE_CACHE_SIZE;
    for (Ext2CachedInode** pp = &inode_cache_[bucket]; *pp != nullptr; pp = &(*pp)->hash_next) {
        Ext2CachedInode* entry = *pp;
        if (entry->ino != ino) {
            continue;
        }
        if (entry->vfs_inode.refcount == 0) {
            *pp = entry->hash_next;
            delete entry;
            --inode_cache_count_;
        } else {
            entry->stale = true;
        }
        return;
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
    // Copy up to min(i_size, buf_size) bytes; no NUL terminator (Linux readlink).
    uint64_t n =
        (static_cast<uint64_t>(d.i_size) < buf_size) ? static_cast<uint64_t>(d.i_size) : buf_size;
    // Fast symlink: NO data block allocated (i_blocks == 0) and target ≤60B
    // inlines in the i_block[] array. Long symlink: target in the i_block[0]
    // data block. i_blocks==0 is the real discriminator -- Cinux sys_symlink
    // always allocates a data block (so its short symlinks are long-format,
    // i_blocks > 0; ext2_links.cpp), while mkfs.ext2 -d / Buildroot store short
    // symlinks as fast (i_blocks == 0). i_size alone would misread Cinux
    // short symlinks (regression caught by symlink_b2 roundtrip test).
    if (d.i_blocks == 0 && static_cast<uint64_t>(d.i_size) <= sizeof(d.i_block)) {
        const uint8_t* src = reinterpret_cast<const uint8_t*>(d.i_block);
        for (uint64_t i = 0; i < n; ++i) {
            buf[i] = static_cast<char>(src[i]);
        }
        return static_cast<int64_t>(n);
    }
    if (d.i_block[0] == 0) {
        return -1;
    }
    KmBuf blk_buf(4096);
    if (!blk_buf || !read_block(d.i_block[0], blk_buf.get())) {
        return -1;
    }
    const uint8_t* src = blk_buf.data();
    for (uint64_t i = 0; i < n; ++i) {
        buf[i] = static_cast<char>(src[i]);
    }
    return static_cast<int64_t>(n);
}

void Ext2::populate_vfs_inode(Ext2CachedInode& cached) {
    const Ext2Inode& disk = cached.disk_inode;

    cached.vfs_inode.ino = cached.ino;

    cached.vfs_inode.size = disk.i_size;

    uint16_t mode_type = disk.i_mode & EXT2_S_IFMT;
    switch (mode_type) {
    case EXT2_S_IFDIR:
        cached.vfs_inode.type = InodeType::Directory;
        cached.vfs_inode.ops  = &dir_ops_;
        break;
    case EXT2_S_IFREG:
        cached.vfs_inode.type = InodeType::Regular;
        cached.vfs_inode.ops  = &file_ops_;
        break;
    case EXT2_S_IFLNK:
        // F-ECO batch 2 / F-USABILITY batch 1a: a symlink now has its own
        // InodeType (was Unknown -- no Symlink enum existed). readlink() yields
        // the target string: fast symlink inlines it in i_block[], long
        // symlink stores it in the i_block[0] data block. ops reuses file_ops_
        // so readlink() resolves.
        cached.vfs_inode.type = InodeType::Symlink;
        cached.vfs_inode.ops  = &file_ops_;
        break;
    default:
        cached.vfs_inode.type = InodeType::Unknown;
        cached.vfs_inode.ops  = nullptr;
        break;
    }

    cached.vfs_inode.fs_private = &cached;

    cached.vfs_inode.mode   = disk.i_mode;
    cached.vfs_inode.uid    = disk.i_uid;
    cached.vfs_inode.gid    = disk.i_gid;
    cached.vfs_inode.nlink  = disk.i_links_count;
    cached.vfs_inode.atime  = disk.i_atime;
    cached.vfs_inode.ctime  = disk.i_ctime;
    cached.vfs_inode.mtime  = disk.i_mtime;
    cached.vfs_inode.blocks = disk.i_blocks;
}

}  // namespace cinux::fs
