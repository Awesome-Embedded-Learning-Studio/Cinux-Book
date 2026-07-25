/**
 * @file kernel/fs/ext2/ext2_links.cpp
 * @brief Ext2 symlink, hard-link, and rename operations
 */

#include <stdint.h>

#include "ext2.hpp"
#include "kernel/lib/kprintf.hpp"

namespace cinux::fs {

cinux::lib::ErrorOr<void> Ext2DirOps::symlink(Inode* dir, const char* name, uint32_t namelen,
                                              const char* target) {
    if (dir == nullptr || name == nullptr || target == nullptr || namelen == 0) {
        return cinux::lib::Error::InvalidArgument;
    }

    Inode* result = ext2_.symlink(static_cast<uint32_t>(dir->ino), name, namelen, target);
    if (result == nullptr) {
        return cinux::lib::Error::AlreadyExists;
    }
    return {};
}

cinux::lib::ErrorOr<void> Ext2DirOps::link(Inode* dir, const char* name, uint32_t namelen,
                                           const Inode* target) {
    if (dir == nullptr || name == nullptr || target == nullptr || namelen == 0) {
        return cinux::lib::Error::InvalidArgument;
    }

    if (!ext2_.link(static_cast<uint32_t>(dir->ino), name, namelen,
                    static_cast<uint32_t>(target->ino))) {
        return cinux::lib::Error::IOError;
    }
    return {};
}

cinux::lib::ErrorOr<void> Ext2DirOps::rename(Inode* src_dir, const char* src_name, uint32_t src_len,
                                             Inode* dst_dir, const char* dst_name,
                                             uint32_t dst_len) {
    if (src_dir == nullptr || src_name == nullptr || dst_dir == nullptr || dst_name == nullptr ||
        src_len == 0 || dst_len == 0) {
        return cinux::lib::Error::InvalidArgument;
    }

    if (!ext2_.rename(static_cast<uint32_t>(src_dir->ino), src_name, src_len,
                      static_cast<uint32_t>(dst_dir->ino), dst_name, dst_len)) {
        return cinux::lib::Error::IOError;
    }
    return {};
}

Inode* Ext2::symlink(uint32_t parent_ino, const char* name, uint32_t name_len, const char* target) {
    if (name == nullptr || name_len == 0 || name_len > EXT2_NAME_MAX || target == nullptr) {
        return nullptr;
    }

    // target is NUL-terminated; compute its length without libc.
    uint32_t target_len = 0;
    while (target[target_len] != '\0') {
        ++target_len;
    }

    if (target_len == 0) {
        return nullptr;
    }

    if (lookup_in_dir(parent_ino, name, name_len) != 0) {
        return nullptr;  // name already exists in parent
    }

    Ext2Inode dir_disk;
    if (!read_disk_inode(parent_ino, dir_disk)) {
        return nullptr;
    }

    uint32_t new_ino = alloc_inode();
    if (new_ino == 0) {
        cinux::lib::kprintf("[EXT2] symlink: no free inodes\n");
        return nullptr;
    }

    // Store the target string in the first data block (i_block[0]); readlink
    // reads it back from there, sized by i_size.
    uint32_t data_blk = alloc_block();
    if (data_blk == 0) {
        cinux::lib::kprintf("[EXT2] symlink: no free blocks\n");
        free_inode(new_ino);
        return nullptr;
    }

    KmBuf scratch(4096);
    if (!scratch) {
        free_block(data_blk);
        free_inode(new_ino);
        return nullptr;
    }

    Ext2Inode new_disk;
    for (uint32_t i = 0; i < sizeof(Ext2Inode); ++i) {
        reinterpret_cast<uint8_t*>(&new_disk)[i] = 0;
    }

    new_disk.i_mode        = EXT2_S_IFLNK | 0777;
    new_disk.i_uid         = 0;
    new_disk.i_size        = target_len;
    new_disk.i_atime       = 0;
    new_disk.i_ctime       = 0;
    new_disk.i_mtime       = 0;
    new_disk.i_dtime       = 0;
    new_disk.i_gid         = 0;
    new_disk.i_links_count = 1;
    new_disk.i_blocks      = block_size_ / 512;
    new_disk.i_flags       = 0;
    new_disk.i_block[0]    = data_blk;

    // Write the target bytes into the data block. readlink reads exactly
    // i_size bytes, so trailing block bytes need not be zeroed.
    auto* dma = scratch.data();
    for (uint32_t i = 0; i < target_len; ++i) {
        dma[i] = static_cast<uint8_t>(target[i]);
    }
    if (!write_block(data_blk, scratch.get())) {
        free_block(data_blk);
        free_inode(new_ino);
        return nullptr;
    }

    if (!write_disk_inode(new_ino, new_disk)) {
        free_block(data_blk);
        free_inode(new_ino);
        return nullptr;
    }

    if (!add_dir_entry(parent_ino, dir_disk, new_ino, name, name_len, Ext2FileType::Symlink)) {
        free_block(data_blk);
        free_inode(new_ino);
        return nullptr;
    }

    if (!write_disk_inode(parent_ino, dir_disk)) {
        return nullptr;
    }

    return get_cached_inode(new_ino);
}

bool Ext2::link(uint32_t parent_ino, const char* name, uint32_t name_len, uint32_t target_ino) {
    if (name == nullptr || name_len == 0 || name_len > EXT2_NAME_MAX) {
        return false;
    }

    if (lookup_in_dir(parent_ino, name, name_len) != 0) {
        return false;  // name already exists in parent
    }

    Ext2Inode dir_disk;
    if (!read_disk_inode(parent_ino, dir_disk)) {
        return false;
    }

    Ext2Inode target_disk;
    if (!read_disk_inode(target_ino, target_disk)) {
        return false;
    }

    target_disk.i_links_count++;
    if (!write_disk_inode(target_ino, target_disk)) {
        return false;
    }

    // Simplified file-type mapping: directories -> Directory, everything else
    // (regular files, symlinks, fifos, sockets) -> Regular. Matches the
    // directory-entry convention used elsewhere in this driver.
    Ext2FileType ft = ((target_disk.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR) ? Ext2FileType::Directory
                                                                           : Ext2FileType::Regular;

    if (!add_dir_entry(parent_ino, dir_disk, target_ino, name, name_len, ft)) {
        // Roll back the link count bump so we don't leak a reference.
        target_disk.i_links_count--;
        write_disk_inode(target_ino, target_disk);
        return false;
    }

    if (!write_disk_inode(parent_ino, dir_disk)) {
        return false;
    }

    // stat(target) reads the cached disk_inode; the nlink bump above is on disk
    // only until we drop the stale cache entry.
    invalidate_cached_inode(target_ino);
    return true;
}

bool Ext2::rename(uint32_t src_dir_ino, const char* src_name, uint32_t src_len,
                  uint32_t dst_dir_ino, const char* dst_name, uint32_t dst_len) {
    if (src_name == nullptr || dst_name == nullptr || src_len == 0 || dst_len == 0) {
        return false;
    }

    Ext2Inode src_disk;
    if (!read_disk_inode(src_dir_ino, src_disk)) {
        return false;
    }

    Ext2Inode dst_disk;
    // Same-directory move: src and dst share one on-disk inode, so read once.
    if (src_dir_ino == dst_dir_ino) {
        dst_disk = src_disk;
    } else {
        if (!read_disk_inode(dst_dir_ino, dst_disk)) {
            return false;
        }
    }

    // Simplified semantics: refuse to overwrite an existing dst entry. (Linux
    // rename(2) replaces it; follow-up.)
    if (lookup_in_dir(dst_dir_ino, dst_name, dst_len) != 0) {
        return false;
    }

    uint32_t ino = 0;
    if (!remove_dir_entry(src_dir_ino, src_disk, src_name, src_len, ino)) {
        cinux::lib::kprintf("[EXT2] rename: '%s' not found\n", src_name);
        return false;
    }

    Ext2Inode target_disk;
    if (!read_disk_inode(ino, target_disk)) {
        return false;
    }

    Ext2FileType ft = ((target_disk.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR) ? Ext2FileType::Directory
                                                                           : Ext2FileType::Regular;

    if (!add_dir_entry(dst_dir_ino, dst_disk, ino, dst_name, dst_len, ft)) {
        return false;
    }

    if (!write_disk_inode(dst_dir_ino, dst_disk)) {
        return false;
    }

    if (src_dir_ino == dst_dir_ino) {
        // src_disk and dst_disk are the same object; dst_disk was mutated by
        // add_dir_entry, so a single write covers both.
        src_disk = dst_disk;
    } else {
        if (!write_disk_inode(src_dir_ino, src_disk)) {
            return false;
        }
    }

    // NOTE (follow-up): renaming a directory should adjust the parent link
    // counts of source and destination parents (".." target changes). This
    // hobby-OS move does not track that; leaving as a known simplification.
    return true;
}

}  // namespace cinux::fs
