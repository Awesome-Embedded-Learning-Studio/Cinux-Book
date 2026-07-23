/**
 * @file kernel/fs/ext2_common.cpp
 * @brief Ext2FileOps and Ext2DirOps implementations
 *
 * VFS InodeOps wrappers that delegate to the Ext2 driver for file
 * read/write/stat and directory readdir/create/mkdir/unlink/stat.
 */

#include <stddef.h>
#include <stdint.h>

#include "ext2.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/lib/string.hpp"

namespace cinux::fs {

// ============================================================
// Ext2FileOps
// ============================================================

Ext2FileOps::Ext2FileOps(Ext2& ext2) : ext2_(ext2) {}

bool Ext2FileOps::is_page_cacheable() const {
    return true;
}

cinux::lib::ErrorOr<int64_t> Ext2FileOps::read(const Inode* inode, uint64_t offset, void* buf,
                                               uint64_t count) {
    if (inode == nullptr || inode->fs_private == nullptr || buf == nullptr) {
        return cinux::lib::Error::InvalidArgument;
    }

    auto*            cached = static_cast<const Ext2CachedInode*>(inode->fs_private);
    const Ext2Inode& disk   = cached->disk_inode;

    if (offset >= disk.i_size) {
        return 0;
    }

    uint64_t available = disk.i_size - offset;
    uint64_t to_read   = (count < available) ? count : available;

    if (to_read == 0) {
        return 0;
    }

    uint32_t bs                   = ext2_.block_size();
    uint64_t block_ptrs_per_block = bs / sizeof(uint32_t);

    auto*    dst        = static_cast<uint8_t*>(buf);
    uint64_t total_read = 0;

    while (total_read < to_read) {
        uint64_t file_block   = (offset + total_read) / bs;
        uint64_t block_offset = (offset + total_read) % bs;
        uint64_t chunk        = bs - block_offset;
        if (chunk > to_read - total_read) {
            chunk = to_read - total_read;
        }

        uint32_t disk_block = 0;

        if (file_block < EXT2_DIRECT_BLOCKS) {
            disk_block = disk.i_block[file_block];
        } else if (file_block < EXT2_DIRECT_BLOCKS + block_ptrs_per_block) {
            uint32_t indirect_block = disk.i_block[EXT2_INDIRECT_BLOCK];
            if (indirect_block == 0) {
                break;
            }

            if (!ext2_.read_block(indirect_block)) {
                break;
            }

            uint32_t idx      = static_cast<uint32_t>(file_block - EXT2_DIRECT_BLOCKS);
            auto*    indirect = reinterpret_cast<uint32_t*>(ext2_.block_buf());
            disk_block        = indirect[idx];
        } else if (file_block < EXT2_DIRECT_BLOCKS + block_ptrs_per_block +
                                    block_ptrs_per_block * block_ptrs_per_block) {
            // Doubly-indirect: i_block[13] points at a block of indirect
            // pointers, each of which points at a block of data pointers.
            uint32_t double_block = disk.i_block[EXT2_DOUBLE_INDIRECT_BLOCK];
            if (double_block == 0) {
                break;
            }
            if (!ext2_.read_block(double_block)) {
                break;
            }

            uint32_t di_offset =
                static_cast<uint32_t>(file_block - EXT2_DIRECT_BLOCKS - block_ptrs_per_block);
            uint32_t idx1           = di_offset / block_ptrs_per_block;
            uint32_t idx2           = di_offset % block_ptrs_per_block;
            auto*    double_ptrs    = reinterpret_cast<uint32_t*>(ext2_.block_buf());
            uint32_t indirect_block = double_ptrs[idx1];
            if (indirect_block == 0) {
                break;
            }
            if (!ext2_.read_block(indirect_block)) {
                break;
            }

            auto* child_ptrs = reinterpret_cast<uint32_t*>(ext2_.block_buf());
            disk_block       = child_ptrs[idx2];
            // disk_block == 0 here means a hole inside an allocated indirect
            // block; the common path below zero-fills it (sparse-file read).
        } else {
            break;
        }

        if (disk_block == 0) {
            for (uint64_t i = 0; i < chunk; ++i) {
                dst[total_read + i] = 0;
            }
            total_read += chunk;
            continue;
        }

        if (!ext2_.read_block(disk_block)) {
            break;
        }

        auto* src = reinterpret_cast<const uint8_t*>(ext2_.block_buf()) + block_offset;
        memcpy(dst + total_read, src, chunk);
        total_read += chunk;
    }

    return static_cast<int64_t>(total_read);
}

cinux::lib::ErrorOr<int64_t> Ext2FileOps::write(Inode* inode, uint64_t offset, const void* buf,
                                                uint64_t count) {
    if (inode == nullptr || inode->fs_private == nullptr || buf == nullptr) {
        return cinux::lib::Error::InvalidArgument;
    }

    if (count == 0) {
        return 0;
    }

    auto*      cached = static_cast<Ext2CachedInode*>(inode->fs_private);
    Ext2Inode& disk   = cached->disk_inode;

    uint32_t bs = ext2_.block_size();

    // Highest logical block we can resolve: direct + single-indirect +
    // double-indirect.  Beyond this (triple-indirect, i_block[14]) the driver
    // does not allocate, so stop instead of silently truncating mid-chunk.
    uint64_t ptrs_per_block = bs / sizeof(uint32_t);
    uint64_t max_file_block = EXT2_DIRECT_BLOCKS + ptrs_per_block + ptrs_per_block * ptrs_per_block;

    auto*    src           = static_cast<const uint8_t*>(buf);
    uint64_t total_written = 0;

    while (total_written < count) {
        uint64_t file_block   = (offset + total_written) / bs;
        uint64_t block_offset = (offset + total_written) % bs;
        uint64_t chunk        = bs - block_offset;
        if (chunk > count - total_written) {
            chunk = count - total_written;
        }

        if (file_block >= max_file_block) {
            break;
        }

        uint32_t disk_block = ext2_.get_or_alloc_block(disk, static_cast<uint32_t>(file_block));
        if (disk_block == 0) {
            cinux::lib::kprintf("[EXT2] file_write: failed to alloc block for file_block %u\n",
                                static_cast<uint32_t>(file_block));
            break;
        }

        if (block_offset != 0 || chunk != bs) {
            if (!ext2_.read_block(disk_block)) {
                break;
            }
        } else {
            auto* dma = reinterpret_cast<uint8_t*>(ext2_.block_buf());
            for (uint32_t i = 0; i < bs; ++i) {
                dma[i] = 0;
            }
        }

        auto* dst = reinterpret_cast<uint8_t*>(ext2_.block_buf()) + block_offset;
        for (uint64_t i = 0; i < chunk; ++i) {
            dst[i] = src[total_written + i];
        }

        if (!ext2_.write_block(disk_block)) {
            break;
        }

        total_written += chunk;
    }

    if (total_written > 0) {
        uint64_t new_end = offset + total_written;
        if (new_end > disk.i_size) {
            disk.i_size = static_cast<uint32_t>(new_end);

            uint32_t sectors_used = ((disk.i_size + bs - 1) / bs) * (bs / 512);
            disk.i_blocks         = sectors_used;
        }

        ext2_.write_disk_inode(static_cast<uint32_t>(inode->ino), disk);

        inode->size = disk.i_size;
    }

    return static_cast<int64_t>(total_written);
}

cinux::lib::ErrorOr<void> Ext2FileOps::stat(const Inode* inode, struct stat* st) {
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

// ============================================================
// F-ECO batch 2 stubs (block A replaces these with real implementations).
// ============================================================

cinux::lib::ErrorOr<void> Ext2FileOps::chmod(Inode* inode, uint32_t mode) {
    if (inode == nullptr) {
        return cinux::lib::Error::InvalidArgument;
    }
    return ext2_.chmod(static_cast<uint32_t>(inode->ino), mode) ? cinux::lib::ErrorOr<void>{}
                                                                : cinux::lib::Error::IOError;
}

cinux::lib::ErrorOr<void> Ext2FileOps::chown(Inode* inode, uint32_t uid, uint32_t gid) {
    if (inode == nullptr) {
        return cinux::lib::Error::InvalidArgument;
    }
    return ext2_.chown(static_cast<uint32_t>(inode->ino), uid, gid) ? cinux::lib::ErrorOr<void>{}
                                                                    : cinux::lib::Error::IOError;
}

cinux::lib::ErrorOr<void> Ext2FileOps::utimensat(Inode* inode, uint64_t atime_sec,
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

cinux::lib::ErrorOr<int64_t> Ext2FileOps::readlink(const Inode* inode, char* buf,
                                                   uint64_t buf_size) {
    if (inode == nullptr || buf == nullptr) {
        return cinux::lib::Error::InvalidArgument;
    }
    int64_t n = ext2_.readlink(static_cast<uint32_t>(inode->ino), buf, buf_size);
    if (n < 0) {
        return cinux::lib::Error::IOError;
    }
    return n;
}

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
