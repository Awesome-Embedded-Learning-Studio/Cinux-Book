/**
 * @file    open_file.hpp
 * @brief   The one open description several descriptors may share.
 *
 * An open file is not the descriptor: the number ring 3 holds is just
 * a seat in one task's table, while the cursor and the node it walks
 * belong to the open itself. Lifting those into a refcounted object
 * lets a forked child name the same open as its parent (one cursor,
 * advanced by whoever reads next) and lets the thread station share
 * the whole table instead of faking it.
 *
 * @author  Charliechen114514
 * @date    2026-10-17
 * @version 0.1
 * @since   0.8.0
 * @ingroup kernel_fs
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <atomic>

#include "cinux/ref_count.hpp"
#include "kernel/fs/fs_config.hpp"
#include "kernel/fs/vfs.hpp"

namespace cinux::fs {

/**
 * @brief   One live open: backend pair, node handle, and the cursor.
 * @note    Owned through base::Shared; the count is created at one and
 *          the last release deletes the object, which drops the node
 *          reference the open has held since birth. Copying a Shared to
 *          it retains; the cursor moves for every owner at once.
 * @since   0.8.0
 * @ingroup kernel_fs
 */
struct OpenFile : base::RefCount<> {
    /**
     * @brief         Names one open on its backend pair; count starts at one.
     * @param[in]     face     The backend's erased face.
     * @param[in]     self     The backend instance.
     * @param[in]     handle   The node handle the open walks.
     * @since         0.8.0
     * @ingroup       kernel_fs
     */
    OpenFile(const FsOps* face, void* self, FsNode handle)
        : ops(face), backend(self), node(handle) {}

    const FsOps*             ops     = nullptr;  ///< The backend's erased face.
    void*                    backend = nullptr;  ///< The backend instance.
    FsNode                   node{};             ///< Which node of that backend.
    unsigned long            offset = 0;  ///< Bytes read so far, or the directory entry ordinal.
    /// The open's own status bits (the O_RDWR/O_APPEND/O_NONBLOCK half
    /// of the open flags): what F_GETFL answers and F_SETFL re-arms.
    unsigned long            status_flags = 0;
    base::Shared<MountLease> lifetime{nullptr};  ///< Keeps a detached runtime backend alive.


    /// Final-close observer, installed atomically by readiness registries.
    std::atomic<void (*)(const OpenFile*)> on_last_close{nullptr};

    /// Drops the node reference this open has carried since birth.
    ~OpenFile() {
        if (const auto kNotify = on_last_close.load(); kNotify != nullptr) {
            kNotify(this);
        }
        if (ops != nullptr) {
            ops->release(backend, node);
        }
    }

    OpenFile(const OpenFile&)            = delete;
    OpenFile& operator=(const OpenFile&) = delete;
    OpenFile(OpenFile&&)                 = delete;
    OpenFile& operator=(OpenFile&&)      = delete;
};

}  // namespace cinux::fs
