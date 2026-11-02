/**
 * @file    ext2.hpp
 * @brief   The disk-backed FileSystem backend: read-write ext2.
 *
 * The first real consumer of the block face: this backend asks a
 * BlockCache for block views and answers the FileSystem concept's
 * faces over them. Inodes are read straight from their group tables on
 * every ask — no inode cache; the block cache covers these reads for
 * free. Every node handle is self-sufficient (an inode number plus
 * metadata). Open descriptions pin inode allocation until their final release.
 *
 * Writes take the mutex, because a whole metadata sequence — find a
 * free bit, set it, discount the counters, write the superblock —
 * must land as one breath or two files can claim the same block.
 * Readers share the volume mutex so staged namespace changes become
 * visible only after publication. The mutex is the sleeping kind on purpose:
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
#include "cinux/container/self_list.hpp"
#include "cinux/math.hpp"
#include "cinux/result.hpp"
#include "cinux/spin_mutex.hpp"
#include "kernel/driver/block/block_device_concept.hpp"
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

    /** @brief Releases the volume's idle writer lock identity. */
    ~Ext2();

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
     * @brief Flushes a complete metadata sequence and waits for the device.
     * @return True when mounted and all writes and the device flush succeeded.
     * @note Serializes against mutations with the volume writer lock.
     */
    [[nodiscard]] bool sync();

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
    /// Resolve and pin the inode in the same volume critical section.
    [[nodiscard]] base::Result<FsNode> lookup_held(std::string_view path);
    /// Create and pin a new inode before releasing the volume critical section.
    [[nodiscard]] base::Result<FsNode> make_file_held(std::string_view path);

    /** @brief Resolves a relative path against one held directory node. */
    [[nodiscard]] base::Result<FsNode> lookup_at(FsNode base, std::string_view rest);
    /** @brief Creates one file directly under one held directory node. */
    [[nodiscard]] base::Result<FsNode> make_file_at(FsNode base, std::string_view name);

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
     * @brief Appends under the backend write lock and reports the final cursor.
     * @param[in] node Regular file handle.
     * @param[in,out] offset Receives the cursor after this write.
     * @param[in] source Kernel bytes to store.
     * @param[in] count Bytes to store.
     * @return Bytes written, or a backend error.
     */
    [[nodiscard]] base::Result<unsigned long> append(FsNode node, unsigned long& offset,
                                                     const void* source, unsigned long count);
    /**
     * @brief Truncates a regular file to zero under the backend write lock.
     * @param[in] node Regular file handle.
     * @return Success or a backend error.
     */
    [[nodiscard]] base::Result<void>          truncate(FsNode node);

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

    /**
     * @brief     Whether a reader would make progress on this node now.
     *
     * @param[in] node   Any handle.
     * @return    True: the volume's bytes are always at hand.
     * @since     0.5.0
     * @ingroup   kernel_fs
     */
    [[nodiscard]] bool readable(FsNode node);

    /**
     * @brief     Answers one device question for this backend's nodes.
     *
     * @param[in] node       Any handle.
     * @param[in] word       The question word.
     * @param[in] argument   The question's argument word.
     * @return    -ENOTTY: no node on a platter is a device.
     * @since     0.5.0
     * @ingroup   kernel_fs
     */
    long long ioctl(FsNode node, unsigned long long word, unsigned long long argument);

    /**
     * @brief     Replaces a node at open time for this backend.
     *
     * @param[in] node    The node lookup produced.
     * @param[in] path    The mount-relative path being opened.
     * @return    The same node: a platter has no cloning opens.
     * @since     0.5.0
     * @ingroup   kernel_fs
     */
    [[nodiscard]] base::Result<FsNode> adopt_open(FsNode node, std::string_view path);

    /** @brief Move a regular file within this volume, replacing a regular target. */
    [[nodiscard]] base::Result<void> rename(std::string_view source, std::string_view target);
    /** @brief Update permission bits while preserving the inode type. */
    [[nodiscard]] base::Result<void> chmod(FsNode node, unsigned int mode);

private:
    [[nodiscard]] base::Result<void>          unlink_locked(std::string_view path);
    [[nodiscard]] base::Result<unsigned long> write_locked(FsNode node, unsigned long offset,
                                                           const void* source, unsigned long count);

    base::Result<Inode> read_child(const Inode& parent, std::string_view name, unsigned long& ino);
    [[nodiscard]] base::Result<FsNode> walk_from(unsigned long start_ino, char* pending);
    /// One walk's redirect inputs: where the walk stands and who links.
    struct LinkSpot {
        std::string_view walked;
        std::string_view part;
        std::string_view rest;
        char*            pending;
        unsigned long    start_ino;
    };
    /// Expands one symlink on the walk, refusing absolute targets.
    [[nodiscard]] base::Result<bool> redirect_through_link(const LinkSpot& spot, const Inode& link,
                                                           unsigned long ino);
    [[nodiscard]] base::Result<FsNode> seat_new_file(unsigned long    parent_ino,
                                                     std::string_view leaf);
    struct SymlinkPlan;
    base::Result<void> expand_symlink(const Inode& inode, unsigned long ino,
                                      const SymlinkPlan& plan);

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

    bool add_dir_entry(Inode& parent_dir, unsigned long parent_ino, unsigned long child_ino,
                       std::string_view name);
    /// Whether one directory entry's lengths and extent stay lawful.
    [[nodiscard]] static bool entry_is_sound(const DirEntry& entry, unsigned long cursor);
    [[nodiscard]] bool        fill_dead_slot(const DirInsert& slot);
    [[nodiscard]] bool        shrink_live_entry(const DirInsert& slot);
    bool                      remove_dir_entry(const Inode& parent_dir, std::string_view name);
    /// Rewrites the inode field of one named directory entry in place.
    [[nodiscard]] bool        replace_dir_entry_ino(const Inode& parent_dir, std::string_view name,
                                                    unsigned long ino);
    /// Drops one link from a file inode, freeing storage at the last.
    [[nodiscard]] base::Result<void> drop_file_link(unsigned long ino);
    /// Whether a directory carries no entries beyond its own two dots.
    [[nodiscard]] base::Result<bool> directory_is_empty(const Inode& victim);
    /// The two parent directories a rename walks between.
    struct RenameEnds {
        FsNode old_parent;
        FsNode new_parent;
        Inode  old_dir;
        Inode  new_dir;
    };
    [[nodiscard]] base::Result<RenameEnds> resolve_rename_ends(std::string_view old_parent,
                                                               std::string_view new_parent);
    /// Seats the new name without retiring anything.
    [[nodiscard]] bool seat_new_name(RenameEnds& ends, std::string_view new_leaf,
                                     unsigned long source_ino, bool replacing);
    /// One rename's naming decisions, resolved before any write.
    struct RenamePlan {
        std::string_view old_leaf;
        std::string_view new_leaf;
        unsigned long    source_ino;
        unsigned long    target_ino;
        bool             replacing;
    };
    /// Stage both directory changes for the enclosing mutation.
    [[nodiscard]] base::Result<void> swap_names(RenameEnds& ends, const RenamePlan& plan);

    /// Retires an emptied directory inode and its parent's back link.
    [[nodiscard]] base::Result<void> retire_empty_directory(unsigned long ino,
                                                            const Inode&  dead_dir,
                                                            unsigned long parent_ino,
                                                            const Inode&  parent);

    class Mutation;
    struct InodeHold : base::container::SelfNode<InodeHold> {
        unsigned long ino   = 0;
        unsigned long count = 0;
    };
    InodeHold*                  held_inode(unsigned long ino);
    bool                        has_hold(unsigned long ino);
    void                        retain_locked(FsNode node);
    base::Result<FsNode>        make_file_locked(std::string_view path);
    bool                        reap_inode(unsigned long ino);
    bool                        reap_pending();
    base::Result<FsNode>        lookup_locked(std::string_view path);
    base::Result<unsigned long> read_locked(FsNode node, unsigned long offset, void* sink,
                                            unsigned long count);
    bool                        read_storage(driver::Lba first, void* sink, unsigned long bytes);
    bool patch_storage(driver::Lba first, unsigned long offset, unsigned long bytes,
                       const void* source);
    base::container::SelfList<InodeHold> held_;
    base::SpinMutex                      hold_gate_;
    Mutation*                            mutation_ = nullptr;

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
