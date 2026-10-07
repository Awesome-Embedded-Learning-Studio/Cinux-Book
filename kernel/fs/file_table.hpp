/**
 * @file    file_table.hpp
 * @brief   One task's open files: slots keyed by descriptor number.
 *
 * A file descriptor is an index, nothing more — the number ring 3 holds
 * names the slot it lands on. Slots 0 through 2 are never handed out:
 * read and write give those three their fixed meanings (keyboard in,
 * console out, console err) before the table is ever asked, so the
 * first allocated descriptor is 3, the shape every Unix program grew up
 * expecting. One slot carries everything a follow-up call needs: which
 * backend, which node of it, and how far this open file has traveled —
 * the offset is bytes for a file and the entry ordinal for a directory,
 * the doubling getdents leans on.
 *
 * @author  Charliechen114514
 * @date    2026-10-07
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_fs
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "cinux/assert.hpp"
#include "cinux/container/slot_table.hpp"
#include "cinux/result.hpp"
#include "kernel/arch/x86_64/irq_guard.hpp"
#include "kernel/fs/vfs.hpp"

namespace cinux::fs {

/**
 * @brief   How many descriptors one task may hold at once.
 * @note    Three reserved standard streams plus five open files is a
 *          teaching kernel's honest budget.
 * @since   0.1.0
 * @ingroup kernel_fs
 */
inline constexpr unsigned long kFileTableMax = 8;

/**
 * @brief   The first descriptor the table hands out.
 * @note    0, 1 and 2 keep their fixed meanings in the syscall layer.
 * @since   0.1.0
 * @ingroup kernel_fs
 */
inline constexpr unsigned long kFirstDescriptor = 3;

/**
 * @brief   One open file: backend pair, node handle, and the cursor.
 * @note    SlotTable owns occupancy; every stored FileSlot names an open file.
 * @since   0.1.0
 * @ingroup kernel_fs
 */
struct FileSlot {
    const FsOps*  ops     = nullptr;  ///< The backend's erased face.
    void*         backend = nullptr;  ///< The backend instance.
    FsNode        node{};             ///< Which node of that backend.
    unsigned long offset = 0;         ///< Bytes read so far, or the directory entry ordinal.
};

/**
 * @brief   Descriptor table owning one node reference per live slot.
 * @note    Lives inside the Task by value; constant-initializable, no
 *          constructor story. alloc always returns the lowest free
 *          slot at or above kFirstDescriptor, so freed numbers are
 *          reused lowest-first.
 * @since   0.1.0
 * @ingroup kernel_fs
 */
struct FileTable {
    /**
     * @brief   Starts with no owned descriptors and no runtime setup.
     * @since   0.1.0
     * @ingroup kernel_fs
     */
    FileTable() = default;

    /**
     * @brief   Releases every node reference still owned by the table.
     * @since   0.1.0
     * @ingroup kernel_fs
     */
    ~FileTable() { close_all(); }

    FileTable(const FileTable&)            = delete;
    FileTable& operator=(const FileTable&) = delete;
    FileTable(FileTable&&)                 = delete;
    FileTable& operator=(FileTable&&)      = delete;

    /**
     * @brief         Reserves the lowest free slot at or above 3.
     *
     * @param[in]     ops      The backend's erased face for the slot.
     * @param[in]     backend  The backend instance for the slot.
     * @param[in]     node     The node handle for the slot.
     * @return        The descriptor number, or kOutOfMemory when every
     *                slot is taken.
     * @note          The caller fills ops, backend and node in one
     *                breath; offset starts at zero. The backend outlives
     *                this table. Lookup through allocation is protected
     *                from unlink by the caller; a successful allocation
     *                retains the node, a full table takes no reference.
     * @since         0.1.0
     * @ingroup       kernel_fs
     */
    [[nodiscard]] base::Result<unsigned long> alloc(const FsOps* ops, void* backend, FsNode node) {
        const arch::IrqGuard kGuard;
        const unsigned long  kFd = slots_.insert(
            FileSlot{.ops = ops, .backend = backend, .node = node, .offset = 0}, kFirstDescriptor);
        if (kFd == kFileTableMax) {
            return base::KernelError::kOutOfMemory;
        }
        if (ops != nullptr) {
            ops->retain(backend, node);
        }
        return kFd;
    }

    /**
     * @brief         Hands out one slot for reading and advancing.
     *
     * @param[in]     descriptor   The descriptor number.
     * @return        The slot, or kInvalidArgument (out of range),
     *                kNotFound (slot is free).
     * @since         0.1.0
     * @ingroup       kernel_fs
     */
    [[nodiscard]] base::Result<FileSlot*> get(unsigned long descriptor) {
        if (descriptor >= kFileTableMax) {
            return base::KernelError::kInvalidArgument;
        }
        auto* const kSlot = slots_.get(descriptor);
        if (kSlot == nullptr) {
            return base::KernelError::kNotFound;
        }
        return kSlot;
    }

    /**
     * @brief         Releases one slot for reuse.
     *
     * @param[in]     descriptor   The descriptor number.
     * @return        Success, or the same errors as get.
     * @since         0.1.0
     * @ingroup       kernel_fs
     */
    [[nodiscard]] base::Result<void> free(unsigned long descriptor) {
        const arch::IrqGuard          kGuard;
        const base::Result<FileSlot*> kSlot = get(descriptor);
        if (!kSlot.ok()) {
            return kSlot.error();
        }
        const FileSlot kClosing = *kSlot.value();
        base::safety::Check(slots_.erase(descriptor), "live file descriptor lost its slot");
        if (kClosing.ops != nullptr) {
            kClosing.ops->release(kClosing.backend, kClosing.node);
        }
        return {};
    }

    /**
     * @brief   Closes all live descriptors, including on task destruction.
     * @note    Repeated calls are harmless; reserved streams own no nodes.
     * @since   0.1.0
     * @ingroup kernel_fs
     */
    void close_all() {
        const arch::IrqGuard kGuard;
        slots_.for_each([this](unsigned long descriptor, [[maybe_unused]] FileSlot& slot) {
            base::safety::Check(free(descriptor).ok(), "live file descriptor refused close");
        });
    }

private:
    base::container::SlotTable<FileSlot, kFileTableMax> slots_;
};

}  // namespace cinux::fs
