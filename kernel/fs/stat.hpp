/**
 * @file kernel/fs/stat.hpp
 * @brief File status structure (struct stat)
 *
 * Defines the stat structure returned by sys_stat / sys_fstat.
 * Layout follows the Linux x86_64 convention where practical.
 *
 * Namespace: cinux::fs
 */

#pragma once

#include <stdint.h>

namespace cinux::fs {

/**
 * @brief File status information returned by stat() / fstat()
 *
 * Layout matches the Linux x86_64 `struct stat` (uapi/asm/stat.h) exactly --
 * 144 bytes -- so musl/glibc user binaries read the right fields. musl on
 * x86_64 consumes this kernel struct directly (no translation); glibc reads it
 * as `struct kernel_stat`. Verified against /usr/include/asm/stat.h and musl
 * arch/x86_64/bits/stat.h.
 *
 * Fields are filled by the filesystem backend from on-disk inode data.
 */
struct stat {
    uint64_t st_dev;         ///< Device ID
    uint64_t st_ino;         ///< Inode number
    uint64_t st_nlink;       ///< Number of hard links (Linux: unsigned long, 8 bytes)
    uint32_t st_mode;        ///< File type and permissions
    uint32_t st_uid;         ///< Owner user ID
    uint32_t st_gid;         ///< Owner group ID
    uint32_t __pad0;         ///< Padding (aligns st_rdev to offset 40, per Linux ABI)
    uint64_t st_rdev;        ///< Device ID (if special file)
    int64_t  st_size;        ///< Total file size in bytes
    int64_t  st_blksize;     ///< Preferred block size for I/O
    int64_t  st_blocks;      ///< Number of 512-byte blocks allocated
    uint64_t st_atime;       ///< Time of last access (seconds)
    uint64_t st_atime_nsec;  ///< Time of last access (nanoseconds)
    uint64_t st_mtime;       ///< Time of last modification (seconds)
    uint64_t st_mtime_nsec;  ///< Time of last modification (nanoseconds)
    uint64_t st_ctime;       ///< Time of last status change (seconds)
    uint64_t st_ctime_nsec;  ///< Time of last status change (nanoseconds)
    int64_t  __unused[3];    ///< Reserved (per Linux ABI)
};

}  // namespace cinux::fs
