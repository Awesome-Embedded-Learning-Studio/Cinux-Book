/**
 * @file    ramfs.hpp
 * @brief   The first FileSystem backend: a writable tree in heap bytes.
 *
 * RamFs satisfies FileSystem with plain methods — the concept check and
 * the mount-table erasure live one floor up, this class knows nothing
 * about either. The whole tree hangs off one root node; directories are
 * sibling-chained children, files keep their bytes in a heap buffer
 * grown page-grain by page-grain with the gap zero-filled, and one
 * spinlock guards everything, held for the whole of each operation —
 * short metadata stretches on a teaching kernel, exactly what the
 * interrupt-blocking fast lane is for.
 *
 * @author  Charliechen114514
 * @date    2026-10-07
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_fs
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <string_view>

#include "cinux/result.hpp"
#include "kernel/fs/vfs.hpp"
#include "kernel/proc/sync.hpp"

namespace cinux::fs {

struct RamNode;

/**
 * @brief   Memory-backed writable file system.
 * @note    The root directory is born with the instance and holds inode
 *          number one; later nodes take monotonically rising numbers.
 *          Borrowed node handles stay valid while linked. Retained
 *          handles also survive unlink until their final release.
 *          The kernel heap's new terminates loudly on
 *          exhaustion, so creation has no quiet-failure path to report.
 * @since   0.1.0
 * @ingroup kernel_fs
 */
class RamFs {
public:
    RamFs();

    ~RamFs();

    RamFs(const RamFs&)            = delete;
    RamFs& operator=(const RamFs&) = delete;

    /**
     * @brief         Finds a node by mount-relative path.
     *
     * @param[in]     path   Path without leading slash; empty addresses
     *                      the root.
     * @return        The node, or kNotFound (no such name),
     *                kNotADirectory (a file sits mid-path).
     * @since         0.1.0
     * @ingroup       kernel_fs
     */
    [[nodiscard]] base::Result<FsNode> lookup(std::string_view path);

    /**
     * @brief         Creates a file at a mount-relative path.
     *
     * @param[in]     path   Where the file appears; parent directories
     *                      must exist, the leaf must not.
     * @return        The fresh node, or kInvalidArgument (empty or
     *                over-long leaf name), kNotFound, kNotADirectory,
     *                kAlreadyExists.
     * @since         0.1.0
     * @ingroup       kernel_fs
     */
    [[nodiscard]] base::Result<FsNode> make_file(std::string_view path);

    /**
     * @brief         Creates a directory at a mount-relative path.
     *
     * @param[in]     path   Where the directory appears; parent
     *                      directories must exist, the leaf must not.
     * @return        The fresh node, or the same errors as make_file.
     * @since         0.1.0
     * @ingroup       kernel_fs
     */
    [[nodiscard]] base::Result<FsNode> make_dir(std::string_view path);

    /**
     * @brief         Removes a file, or a directory that holds nothing.
     *
     * @param[in]     path   Node to remove; the root is refused.
     * @return        Success, or kInvalidArgument (root or trailing
     *                slash), kNotFound, kDirectoryNotEmpty.
     * @since         0.1.0
     * @ingroup       kernel_fs
     */
    [[nodiscard]] base::Result<void> unlink(std::string_view path);

    /**
     * @brief         Reads file bytes at an offset.
     *
     * @param[in]     node    Handle from lookup or creation.
     * @param[in]     offset  First byte to read.
     * @param[out]    sink    Destination buffer.
     * @param[in]     count   Bytes wanted; the tail is clamped to size.
     * @return        Bytes read; zero when offset is at or past the end.
     *                kIsADirectory when node is a directory.
     * @since         0.1.0
     * @ingroup       kernel_fs
     */
    [[nodiscard]] base::Result<unsigned long> read(FsNode node, unsigned long offset, void* sink,
                                                   unsigned long count);

    /**
     * @brief         Writes file bytes at an offset, growing the buffer.
     *
     * @param[in]     node    Handle from lookup or creation.
     * @param[in]     offset  First byte to overwrite; may sit past the
     *                        current end, the gap reads back as zeros.
     * @param[in]     source  Bytes to store.
     * @param[in]     count   Bytes to store.
     * @return        Bytes written (always count).
     *                kIsADirectory when node is a directory.
     * @note          Growth rounds the needed size up to whole
     *               kRamfsGrowthAlign chunks; a hole between the old end
     *               and offset is zero-filled, never stale heap bytes.
     * @since         0.1.0
     * @ingroup       kernel_fs
     */
    [[nodiscard]] base::Result<unsigned long> write(FsNode node, unsigned long offset,
                                                    const void* source, unsigned long count);

    /**
     * @brief         Hands out one directory entry by ordinal.
     *
     * @param[in]     node    Directory handle.
     * @param[in]     index   Which child, zero-based, creation order.
     * @param[out]    entry   Name and type of that child.
     * @return        True with entry filled; false when index is past
     *                the last child. kNotADirectory when node is a file.
     * @since         0.1.0
     * @ingroup       kernel_fs
     */
    [[nodiscard]] base::Result<bool> read_dir(FsNode node, unsigned long index, FsDirent& entry);

    /**
     * @brief   Keeps a node alive for an open descriptor.
     * @param[in] node   A valid borrowed or already retained handle.
     * @note    The caller protects lookup through retain from unlink.
     * @since   0.1.0
     * @ingroup kernel_fs
     */
    void retain(FsNode node);

    /**
     * @brief   Releases an open descriptor's reference to a node.
     * @param[in] node   Handle previously passed to retain.
     * @note    Unlinked nodes are destroyed when their last reference ends.
     * @since   0.1.0
     * @ingroup kernel_fs
     */
    void release(FsNode node);

private:
    RamNode*       root_     = nullptr;
    unsigned long  next_ino_ = 2;
    proc::Spinlock lock_;
};

}  // namespace cinux::fs
