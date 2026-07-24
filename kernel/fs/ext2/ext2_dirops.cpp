/**
 * @file kernel/fs/ext2/ext2_dirops.cpp
 * @brief Ext2DirOps implementation -- VFS InodeOps for ext2 directories
 *
 * readdir/create/mkdir/unlink/stat/chmod/chown/utimensat delegate to the Ext2
 * driver. Split out of ext2_common.cpp (65d750a replay, tag080 Step3) to keep
 * each translation unit under the CI 500-line limit.
 */

#include <stddef.h>
#include <stdint.h>

#include "ext2.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/lib/string.hpp"

namespace cinux::fs {

// ============================================================
// Ext2DirOps
// ============================================================

Ext2DirOps::Ext2DirOps(Ext2& ext2) : ext2_(ext2) {}

cinux::lib::ErrorOr<int64_t> Ext2DirOps::readdir(const Inode* inode, uint64_t index, char* name,
                                                 uint64_t name_max) {
    if (inode == nullptr || inode->fs_private == nullptr || name == nullptr || name_max == 0) {
        return cinux::lib::Error::InvalidArgument;
    }

    auto*            cached = static_cast<const Ext2CachedInode*>(inode->fs_private);
    const Ext2Inode& disk   = cached->disk_inode;

    uint32_t bs = ext2_.block_size();

    if (index == 0) {
        if (name_max < 2) {
            return cinux::lib::Error::InvalidArgument;
        }
        name[0] = '.';
        name[1] = '\0';
        return 1;
    }
    if (index == 1) {
        if (name_max < 3) {
            return cinux::lib::Error::InvalidArgument;
        }
        name[0] = '.';
        name[1] = '.';
        name[2] = '\0';
        return 1;
    }

    uint64_t target   = index - 2;
    uint64_t found    = 0;
    uint64_t dir_size = disk.i_size;

    uint32_t total_blocks = static_cast<uint32_t>((dir_size + bs - 1) / bs);
    if (total_blocks > EXT2_DIRECT_BLOCKS) {
        total_blocks = EXT2_DIRECT_BLOCKS;
    }

    for (uint32_t b = 0; b < total_blocks; ++b) {
        uint32_t blk = disk.i_block[b];
        if (blk == 0) {
            continue;
        }

        if (!ext2_.read_block(blk)) {
            return cinux::lib::Error::IOError;
        }

        auto*    block_data = reinterpret_cast<const uint8_t*>(ext2_.block_buf());
        uint32_t pos        = 0;

        while (pos < bs) {
            if (pos + EXT2_DIR_ENTRY_HDR_SIZE > bs) {
                break;
            }

            auto* entry = reinterpret_cast<const Ext2DirEntry*>(block_data + pos);

            if (entry->rec_len == 0) {
                break;
            }

            if (entry->inode != 0) {
                // Skip "." and ".." — they are handled by the
                // hardcoded indices 0 and 1 above
                if (entry->name_len == 1 && entry->name[0] == '.') {
                    pos += entry->rec_len;
                    continue;
                }
                if (entry->name_len == 2 && entry->name[0] == '.' && entry->name[1] == '.') {
                    pos += entry->rec_len;
                    continue;
                }

                if (found == target) {
                    uint32_t copy_len = static_cast<uint32_t>(name_max) - 1;
                    if (entry->name_len < copy_len) {
                        copy_len = entry->name_len;
                    }

                    for (uint32_t i = 0; i < copy_len; ++i) {
                        name[i] = entry->name[i];
                    }
                    name[copy_len] = '\0';
                    return 1;
                }
                ++found;
            }

            pos += entry->rec_len;
        }
    }

    return 0;
}

cinux::lib::ErrorOr<Inode*> Ext2DirOps::create(Inode* dir, const char* name, uint32_t namelen) {
    if (dir == nullptr || name == nullptr || namelen == 0) {
        return cinux::lib::Error::InvalidArgument;
    }

    Inode* result = ext2_.create(static_cast<uint32_t>(dir->ino), name, namelen);
    if (result == nullptr) {
        return cinux::lib::Error::AlreadyExists;
    }
    return result;
}

cinux::lib::ErrorOr<Inode*> Ext2DirOps::mkdir(Inode* dir, const char* name, uint32_t namelen) {
    if (dir == nullptr || name == nullptr || namelen == 0) {
        return cinux::lib::Error::InvalidArgument;
    }

    Inode* result = ext2_.mkdir(static_cast<uint32_t>(dir->ino), name, namelen);
    if (result == nullptr) {
        return cinux::lib::Error::AlreadyExists;
    }
    return result;
}

cinux::lib::ErrorOr<void> Ext2DirOps::unlink(Inode* dir, const char* name, uint32_t namelen) {
    if (dir == nullptr || name == nullptr || namelen == 0) {
        return cinux::lib::Error::InvalidArgument;
    }

    if (ext2_.unlink(static_cast<uint32_t>(dir->ino), name, namelen) != 0) {
        return cinux::lib::Error::IOError;
    }
    return {};
}

cinux::lib::ErrorOr<void> Ext2DirOps::stat(const Inode* inode, struct stat* st) {
    if (inode == nullptr || inode->fs_private == nullptr || st == nullptr) {
        return cinux::lib::Error::InvalidArgument;
    }

    auto*            cached = static_cast<const Ext2CachedInode*>(inode->fs_private);
    const Ext2Inode& disk   = cached->disk_inode;

    // Zero first so the Linux-ABI fields the backend does not set (__pad0,
    // *_nsec, __unused) stay 0 -- no kernel-stack bytes leak to user space.
    memset(st, 0, sizeof(*st));
    st->st_dev     = 0;
    st->st_ino     = inode->ino;
    st->st_nlink   = disk.i_links_count;
    st->st_mode    = disk.i_mode;
    st->st_uid     = disk.i_uid;
    st->st_gid     = disk.i_gid;
    st->st_rdev    = 0;
    st->st_size    = disk.i_size;
    st->st_blksize = ext2_.block_size();
    st->st_blocks  = disk.i_blocks;
    st->st_atime   = disk.i_atime;
    st->st_mtime   = disk.i_mtime;
    st->st_ctime   = disk.i_ctime;

    return {};
}

// F-ECO batch 2 directory attribute ops. A directory is just an inode; the
// on-disk setattr path is identical to the file case, so we delegate straight
// to the same Ext2 primitives.
cinux::lib::ErrorOr<void> Ext2DirOps::chmod(Inode* inode, uint32_t mode) {
    if (inode == nullptr) {
        return cinux::lib::Error::InvalidArgument;
    }
    return ext2_.chmod(static_cast<uint32_t>(inode->ino), mode) ? cinux::lib::ErrorOr<void>{}
                                                                : cinux::lib::Error::IOError;
}

cinux::lib::ErrorOr<void> Ext2DirOps::chown(Inode* inode, uint32_t uid, uint32_t gid) {
    if (inode == nullptr) {
        return cinux::lib::Error::InvalidArgument;
    }
    return ext2_.chown(static_cast<uint32_t>(inode->ino), uid, gid) ? cinux::lib::ErrorOr<void>{}
                                                                    : cinux::lib::Error::IOError;
}

cinux::lib::ErrorOr<void> Ext2DirOps::utimensat(Inode* inode, uint64_t atime_sec,
                                                uint32_t atime_nsec, uint64_t mtime_sec,
                                                uint32_t mtime_nsec) {
    if (inode == nullptr) {
        return cinux::lib::Error::InvalidArgument;
    }
    return ext2_.utimensat(static_cast<uint32_t>(inode->ino), atime_sec, atime_nsec, mtime_sec,
                           mtime_nsec)
               ? cinux::lib::ErrorOr<void>{}
               : cinux::lib::Error::IOError;
}

}  // namespace cinux::fs
