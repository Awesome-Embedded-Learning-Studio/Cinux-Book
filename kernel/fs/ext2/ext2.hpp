/**
 * @file    ext2.hpp
 * @brief   The disk-backed FileSystem backend: read-write ext2.
 *
 * The first real consumer of the block face: this backend asks a
 * BlockCache for block views and answers the FileSystem concept's
 * faces over them. Inodes are read straight from their group tables on
 * every ask — no inode cache; the block cache covers these reads for
 * free. Every node handle is self-sufficient (an inode number plus
 * metadata), so retain and release have nothing to pin yet.
 *
 * Writes take the mutex, because a whole metadata sequence — find a
 * free bit, set it, discount the counters, write the superblock —
 * must land as one breath or two files can claim the same block.
 * Reads still carry no lock: the kernel's file world serves a single
 * task at a time today, so no reader ever meets a half-written
 * sequence. The day a second task shares this volume, the read faces
 * join the same mutex. The mutex is the sleeping kind on purpose:
 * its critical section crosses queue commands, and a spin lock held
 * across a sleep is this tree's best-documented death.
 *
 * @author  Charliechen114514
 * @date    2026-10-11
 * @version 0.3
 * @since   0.1.0
 * @ingroup kernel_fs
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <cstdint>
#include <string_view>

#include "cinux/bit_ops/bitmap.hpp"
#include "cinux/math.hpp"
#include "cinux/result.hpp"
#include "kernel/fs/ext2/ext2_layout.hpp"
#include "kernel/fs/vfs.hpp"
#include "kernel/proc/sync.hpp"

namespace cinux::driver {
class BlockCache;
}

namespace cinux::fs::ext2 {

/**
 * @brief   Read-write ext2 volume behind one block queue.
 * @note    mount() accepts the teaching volume's shape and nothing
 *          else: 1 KiB blocks, an inode size that both covers the 128
 *          bytes this backend parses and divides the block evenly, no
 *          incompatible features — a volume saying otherwise is
 *          refused loudly rather than half-understood. Paths arrive
 *          mount-relative without a leading slash; the empty path is
 *          the root. A directory's node reports size zero, per the
 *          mount table's contract.
 * @since   0.1.0
 * @ingroup kernel_fs
 */
class Ext2 {
public:
    /// A wall-clock reading, Unix seconds; the backend never reads the
    /// chip itself, it stamps what it is handed.
    using ClockFn = unsigned long long (*)();

    /**
     * @brief         Binds the backend to its block supply and its clock.
     *
     * @param[in,out] cache   The write-through cache over the volume's
     *                        queue; task contexts only, per the
     *                        queue's own contract.
     * @param[in]     clock   Where fresh timestamps come from; a null
     *                        clock stamps zero and deletion falls back
     *                        to the volume's birth time, which keeps
     *                        fsck quiet on machines without a clock.
     * @since         0.1.0
     * @ingroup       kernel_fs
     */
    explicit Ext2(driver::BlockCache& cache, ClockFn clock = nullptr);

    Ext2(const Ext2&)            = delete;
    Ext2& operator=(const Ext2&) = delete;

    /**
     * @brief         Reads and validates the volume's opening tables.
     *
     * @return        Success, or kIOError (unreadable, bad magic,
     *                broken counts), kNotSupported (a shape this
     *                backend refuses), kInvalidArgument (mounted
     *                twice).
     * @note          Must run once before any other face, inside the
     *                single bring-up task that owns the volume.
     * @since         0.1.0
     * @ingroup       kernel_fs
     */
    [[nodiscard]] base::Result<void> mount();

    /**
     * @brief         Finds a node by mount-relative path.
     *
     * @param[in]     path   Path without leading slash; empty addresses
     *                      the root.
     * @return        The node, or kNotFound, kNotADirectory (a file
     *                      sits mid-path), kIOError (unreadable or
     *                      inconsistent metadata), kInvalidArgument
     *                      (a directory names an impossible inode).
     * @since         0.1.0
     * @ingroup       kernel_fs
     */
    [[nodiscard]] base::Result<FsNode> lookup(std::string_view path);

    /**
     * @brief         Creates a file at a mount-relative path.
     *
     * @param[in]     path   Where the file appears; parent directories
     *                      must exist, the leaf must not.
     * @return        The fresh node, or kInvalidArgument (over-long
     *                      leaf), kNotFound, kNotADirectory,
     *                      kAlreadyExists, kOutOfMemory (nothing free),
     *                      kIOError (device or bitmap refused).
     * @since         0.1.0
     * @ingroup       kernel_fs
     */
    [[nodiscard]] base::Result<FsNode> make_file(std::string_view path);

    /**
     * @brief         Creates a directory at a mount-relative path.
     *
     * @param[in]     path   Where the directory appears; parents must
     *                      exist, the leaf must not. The root is
     *                      refused.
     * @return        The fresh node, or the same errors as make_file.
     * @since         0.1.0
     * @ingroup       kernel_fs
     */
    [[nodiscard]] base::Result<FsNode> make_dir(std::string_view path);

    /**
     * @brief         Removes a file, or a directory that holds nothing
     *                but its dot entries.
     *
     * @param[in]     path   Node to remove; the root is refused.
     * @return        Success, or kInvalidArgument (root),
     *                kNotFound, kNotADirectory, kDirectoryNotEmpty,
     *                kOutOfMemory, kIOError.
     * @note          The directory's data blocks are not returned to
     *                the bitmap, matching the reference tools: the
     *                entry vanishes, the space waits for the fsck
     *                story of a later station.
     * @since         0.1.0
     * @ingroup       kernel_fs
     */
    [[nodiscard]] base::Result<void> unlink(std::string_view path);

    /**
     * @brief         Reads file bytes at an offset.
     *
     * @param[in]     node    Handle from lookup.
     * @param[in]     offset  First byte to read.
     * @param[out]    sink    Destination buffer.
     * @param[in]     count   Bytes wanted; the tail is clamped to size.
     * @return        Bytes read; zero at or past the end. Holes read
     *                back as zeros. kIsADirectory on a directory,
     *                kNotSupported past the double-indirect layer,
     *                kIOError when the disk or the metadata refuses.
     * @since         0.1.0
     * @ingroup       kernel_fs
     */
    [[nodiscard]] base::Result<unsigned long> read(FsNode node, unsigned long offset, void* sink,
                                                   unsigned long count);

    /**
     * @brief         Writes file bytes at an offset, growing the file.
     *
     * @param[in]     node    Handle from lookup.
     * @param[in]     offset  First byte to overwrite; a gap past the
     *                        current end reads back as zeros.
     * @param[in]     source  Bytes to store.
     * @param[in]     count   Bytes to store.
     * @return        Bytes written (always count), or kIsADirectory,
     *                kNotSupported past the double-indirect layer,
     *                kOutOfMemory, kIOError.
     * @since         0.1.0
     * @ingroup       kernel_fs
     */
    [[nodiscard]] base::Result<unsigned long> write(FsNode node, unsigned long offset,
                                                    const void* source, unsigned long count);

    /**
     * @brief         Hands out one directory entry by ordinal.
     *
     * @param[in]     node    Directory handle.
     * @param[in]     index   Which entry, zero-based in on-disk order.
     * @param[out]    entry   Name and type of that entry.
     * @return        True with entry filled; false past the last entry.
     *                kNotADirectory on a file, kIOError when the disk
     *                or the metadata refuses, kInvalidArgument on an
     *                impossible inode number.
     * @note          "." and ".." are real entries here and show up
     *          like any other; a directory's kind is read from the
     *          child's inode mode, and names too long for kFsNameMax
     *          are skipped without taking an ordinal.
     * @since         0.1.0
     * @ingroup       kernel_fs
     */
    [[nodiscard]] base::Result<bool> read_dir(FsNode node, unsigned long index, FsDirent& entry);

    /**
     * @brief         Reads one node's metadata.
     *
     * @param[in]     node    Handle from lookup or creation.
     * @param[out]    probed  The metadata as it stands on the platter.
     * @return        Success, or kIOError.
     * @since         0.3.0
     * @ingroup       kernel_fs
     */
    [[nodiscard]] base::Result<void> stat(FsNode node, FsStat& probed);

    /**
     * @brief   Keeps a node alive for an open descriptor.
     * @param[in] node   Handle from lookup.
     * @note    Nothing to pin: the volume holds every inode in its
     *          tables, and freeing returns the number to the bitmap.
     * @since   0.1.0
     * @ingroup kernel_fs
     */
    void retain(FsNode node);

    /**
     * @brief   Releases an open descriptor's reference to a node.
     * @param[in] node   Handle from lookup.
     * @note    Nothing to unpin; the unlink path frees eagerly.
     * @since   0.1.0
     * @ingroup kernel_fs
     */
    void release(FsNode node);

private:
    /// One directory-block spot an insert might land in: where the block
    /// lives, where the spot starts, the entry sitting there, and who is
    /// moving in.
    struct DirInsert {
        unsigned long    disk_block;
        unsigned long    cursor;
        const DirEntry*  entry;
        unsigned long    child;
        std::string_view name;
    };

    [[nodiscard]] bool read_block(unsigned long block, void* sink);

    [[nodiscard]] base::Result<Inode> read_inode(unsigned long ino);

    [[nodiscard]] base::Result<unsigned long> resolve_data_block(const Inode&  node,
                                                                 unsigned long file_block);

    template <typename Visit>
    base::Result<bool> walk_entries(const Inode& directory, Visit&& visit) {
        if (!IsDirectoryMode(directory.i_mode)) {
            return base::KernelError::kNotADirectory;
        }
        const unsigned long kSpan = base::math::Ceil(directory.i_size, block_size_);
        std::uint32_t       block[kBlockWords];
        for (unsigned long file_block = 0; file_block < kSpan; ++file_block) {
            const base::Result<unsigned long> kDisk = resolve_data_block(directory, file_block);
            if (!kDisk.ok()) {
                return kDisk.error();
            }
            if (kDisk.value() == 0) {
                continue;
            }
            if (!read_block(kDisk.value(), block)) {
                return base::KernelError::kIOError;
            }
            unsigned long cursor = 0;
            while (cursor + kDirEntryHeadBytes <= block_size_) {
                const DirEntry& entry = *reinterpret_cast<const DirEntry*>(
                    reinterpret_cast<const unsigned char*>(block) + cursor);
                if (entry.rec_len < kDirEntryHeadBytes || entry.rec_len % kDirEntryAlign != 0 ||
                    cursor + kDirEntryHeadBytes + entry.name_len > block_size_ ||
                    kDirEntryHeadBytes + entry.name_len > entry.rec_len) {
                    return base::KernelError::kIOError;
                }
                if (entry.inode != 0 && !visit(entry)) {
                    return true;
                }
                cursor += entry.rec_len;
            }
        }
        return false;
    }

    [[nodiscard]] base::Result<unsigned long> lookup_in_dir(const Inode&     directory,
                                                            std::string_view name);

    [[nodiscard]] bool bitmap_words(unsigned long bitmap_block, base::bit::WordMask* words,
                                    unsigned long capacity);

    [[nodiscard]] bool patch_group_field(unsigned long group, unsigned long offset,
                                         std::uint16_t value);

    [[nodiscard]] bool adjust_free_blocks(unsigned long group, long delta);
    [[nodiscard]] bool adjust_free_inodes(unsigned long group, long delta);
    [[nodiscard]] bool adjust_used_dirs(unsigned long group, long delta);

    [[nodiscard]] base::Result<unsigned long> alloc_block();
    [[nodiscard]] base::Result<unsigned long> alloc_inode(bool directory);
    void                                      free_block(unsigned long block);
    void                                      free_inode(unsigned long ino);
    void                                      free_data_blocks(const Inode& node);
    void                                      free_direct_blocks(const Inode& node);
    void                                      free_single_indirect(const Inode& node);
    void                                      free_double_indirect(const Inode& node);

    [[nodiscard]] bool write_inode(unsigned long ino, const Inode& inode);

    [[nodiscard]] base::Result<unsigned long> zero_fresh_block();
    [[nodiscard]] base::Result<unsigned long> ensure_data_block(Inode&        node,
                                                                unsigned long file_block);
    [[nodiscard]] base::Result<unsigned long> ensure_direct_block(Inode& node, unsigned long slot);
    [[nodiscard]] base::Result<unsigned long> ensure_single_block(Inode& node, unsigned long outer);
    [[nodiscard]] base::Result<unsigned long> ensure_double_block(Inode& node, unsigned long outer,
                                                                  unsigned long inner);

    bool add_dir_entry(const Inode& parent_dir, unsigned long child_ino, std::string_view name);
    [[nodiscard]] bool fill_dead_slot(const DirInsert& slot);
    [[nodiscard]] bool shrink_live_entry(const DirInsert& slot);
    bool               remove_dir_entry(const Inode& parent_dir, std::string_view name);

    driver::BlockCache& cache_;

    Superblock      sb_{};
    unsigned long   block_size_         = 0;
    unsigned long   group_count_        = 0;
    GroupDescriptor groups_[kMaxGroups] = {};
    bool            mounted_            = false;
    ClockFn         clock_              = nullptr;

    /// The clock's word truncated to the on-disk field; zero without a clock.
    [[nodiscard]] std::uint32_t now_stamp();

    proc::Mutex write_gate_;
};

}  // namespace cinux::fs::ext2
