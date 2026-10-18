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

#include "cinux/bit_ops/bitmap.hpp"
#include "cinux/container/slot_table.hpp"
#include "cinux/ref_count.hpp"
#include "cinux/result.hpp"
#include "cinux/shared.hpp"
#include "kernel/fs/open_file.hpp"
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
 * @brief   One task's open files: shared opens keyed by descriptor number.
 * @note    The table itself is heap-owned and shareable: a task is born
 *          with one, a forked child gets a fresh table seeding every
 *          inherited open by adopt, and a CLONE_FILES thread group
 *          points at a single table so every member sees the same
 *          descriptors and the same cursors. alloc always returns the
 *          lowest free slot at or above kFirstDescriptor, so freed
 *          numbers are reused lowest-first. adopt seats an open another
 *          table already owns, which is how a forked child inherits
 *          its parent's cursor. SlotTable owns occupancy; every stored
 *          entry holds one owner count over its OpenFile.
 * @since   0.1.0
 * @ingroup kernel_fs
 */
struct FileTable : cinux::base::RefCount<> {
    /**
     * @brief   Starts with no owned descriptors and no runtime setup.
     * @since   0.1.0
     * @ingroup kernel_fs
     */
    FileTable();

    /**
     * @brief   Releases every open this table still owns.
     * @since   0.1.0
     * @ingroup kernel_fs
     */
    ~FileTable();

    FileTable(const FileTable&)            = delete;
    FileTable& operator=(const FileTable&) = delete;
    FileTable(FileTable&&)                 = delete;
    FileTable& operator=(FileTable&&)      = delete;

    /**
     * @brief         Opens a fresh description and reserves the lowest
     *                free slot at or above 3 for it.
     *
     * @param[in]     ops      The backend's erased face for the open.
     * @param[in]     backend  The backend instance for the open.
     * @param[in]     node     The node handle for the open.
     * @return        The descriptor number, or kOutOfMemory when every
     *                slot is taken.
     * @note          offset starts at zero. The backend outlives this
     *                table. Lookup through allocation is protected from
     *                unlink by the caller; a successful allocation
     *                retains the node inside the new OpenFile, a full
     *                table takes no reference.
     * @since         0.1.0
     * @ingroup       kernel_fs
     */
    [[nodiscard]] base::Result<unsigned long> alloc(const FsOps* ops, void* backend, FsNode node);

    /**
     * @brief         Seats one open another table already owns.
     *
     * @param[in]     file   The shared open to adopt; this table takes
     *                       one more owner count over it.
     * @return        The descriptor number, or kOutOfMemory when every
     *                slot is taken, or kInvalidArgument when nothing is
     *                held.
     * @note          No node operation runs: the open keeps its single
     *                node reference no matter how many tables point at
     *                it. The cursor is shared from here on.
     * @since         0.8.0
     * @ingroup       kernel_fs
     */
    [[nodiscard]] base::Result<unsigned long> adopt(const base::Shared<OpenFile>& file);

    /**
     * @brief         Seats one open at the descriptor number the caller
     *                names; the slot must be free or empty-handed.
     *
     * @param[in]     file        The shared open to seat.
     * @param[in]     descriptor  The exact number it must answer to.
     * @return        The descriptor, or kInvalidArgument for a free-less
     *                number, kAlreadyExists when one stands there.
     * @note          The seat starts clean: any close-on-exec bit under
     *                the number clears, which is what a dup-shaped
     *                re-seat means.
     * @since         0.9.0
     * @ingroup       kernel_fs
     */
    [[nodiscard]] base::Result<unsigned long> adopt_at(const base::Shared<OpenFile>& file,
                                                       unsigned long                 descriptor);

    /**
     * @brief         Seats one open at the lowest free descriptor at or
     *                above the floor the caller names.
     *
     * @param[in]     file   The shared open to seat.
     * @param[in]     floor  The lowest number the new seat may take.
     * @return        The descriptor number, or kInvalidArgument when
     *                nothing is held, kOutOfMemory when no seat at or
     *                above the floor is free.
     * @note          Floors below the first handed-out number clamp up
     *                to it, so the reserved stream numbers never take a
     *                seat. A dup lands here.
     * @since         0.9.0
     * @ingroup       kernel_fs
     */
    [[nodiscard]] base::Result<unsigned long> adopt_from(const base::Shared<OpenFile>& file,
                                                         unsigned long                 floor);

    /**
     * @brief         Hands out one open for reading and advancing.
     *
     * @param[in]     descriptor   The descriptor number.
     * @return        The open, or kInvalidArgument (out of range),
     *                kNotFound (slot is free).
     * @since         0.1.0
     * @ingroup       kernel_fs
     */
    [[nodiscard]] base::Result<OpenFile*> get(unsigned long descriptor);

    /**
     * @brief         Hands out one owned hold over the open.
     *
     * @param[in]     descriptor   The descriptor number.
     * @return        A Shared retaining the open, or the same errors as
     *                get.
     * @note          The hold keeps the open alive across tables: this
     *                is how a parent hands its cursor to a forked child.
     * @since         0.8.0
     * @ingroup       kernel_fs
     */
    [[nodiscard]] base::Result<base::Shared<OpenFile>> shared(unsigned long descriptor);

    /**
     * @brief         Releases one descriptor for reuse.
     *
     * @param[in]     descriptor   The descriptor number.
     * @return        Success, or the same errors as get.
     * @since         0.1.0
     * @ingroup       kernel_fs
     */
    [[nodiscard]] base::Result<void> free(unsigned long descriptor);

    /**
     * @brief   Closes all live descriptors, including on task destruction.
     * @note    Repeated calls are harmless; reserved streams own no opens.
     * @since   0.1.0
     * @ingroup kernel_fs
     */
    void close_all();

    /**
     * @brief         Arms or disarms one descriptor's close-on-exec bit.
     * @param[in]     descriptor   The descriptor number.
     * @param[in]     armed        True arms the bit, false clears it.
     * @return        Success, or kInvalidArgument when the number is
     *                out of the table's range.
     * @since         0.9.0
     * @ingroup       kernel_fs
     */
    [[nodiscard]] base::Result<void> set_cloexec(unsigned long descriptor, bool armed);

    /**
     * @brief         Reports one descriptor's close-on-exec bit.
     * @param[in]     descriptor   The descriptor number.
     * @return        True when armed; a number out of range answers false.
     * @since         0.9.0
     * @ingroup       kernel_fs
     */
    [[nodiscard]] bool cloexec(unsigned long descriptor) const;

    /**
     * @brief   Strips every armed close-on-exec bit after a successful exec.
     * @note    The exec tail walks the whole table; each armed bit also
     *          releases the open under it, which is the whole point.
     * @since   0.9.0
     * @ingroup kernel_fs
     */
    void close_on_exec();

private:
    base::container::SlotTable<base::Shared<OpenFile>, kFileTableMax> slots_;
    /// One bit per descriptor number: set means close at exec. The
    /// bitmap owns the words; the array is just the storage it binds.
    base::bit::WordMask cloexec_words_[base::bit::Bitmap::words_for(kFileTableMax)] = {};
    base::bit::Bitmap   cloexec_map_{};
};

}  // namespace cinux::fs
