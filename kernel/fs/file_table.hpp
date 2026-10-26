/**
 * @file    file_table.hpp
 * @brief   One task's open files: slots keyed by descriptor number.
 *
 * Descriptor numbers select shared open descriptions. Storage grows in
 * bitmap-sized chunks, while the caller supplies its current resource
 * ceiling. Standard streams occupy ordinary slots; once closed, zero
 * through two can be reused by the same lowest-free rule as every other
 * descriptor. Shared tables serialize lookup and replacement, and release
 * backend ownership after dropping the table lock.
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
#include "cinux/own.hpp"
#include "cinux/ref_count.hpp"
#include "cinux/result.hpp"
#include "cinux/shared.hpp"
#include "cinux/spin_mutex.hpp"
#include "kernel/fs/open_file.hpp"
#include "kernel/fs/vfs.hpp"

namespace cinux::fs {

/** @brief Linux's default nr_open ceiling; storage grows only when used. */
inline constexpr unsigned long kFileTableMax = 1024UL * 1024;

/** @brief Open metadata initialized before a descriptor becomes visible. */
struct OpenOptions {
    unsigned long            status  = 0;
    bool                     cloexec = false;
    base::Shared<MountLease> lifetime{nullptr};  ///< Backend ownership inherited from resolve.
    unsigned long limit = kFileTableMax;  ///< Exclusive descriptor ceiling for this allocation.
};

/**
 * @brief   One task's open files: shared opens keyed by descriptor number.
 * @note    The table itself is heap-owned and shareable: a task is born
 *          with one, a forked child gets a fresh table seeding every
 *          inherited open by adopt, and a CLONE_FILES thread group
 *          points at a single table so every member sees the same
 *          descriptors and the same cursors. alloc always returns the
 *          lowest free slot, zero included: seats zero through two are
 *          reserved not by the table but by the standard streams a task
 *          inherits, so a born-empty table's first open takes seat zero
 *          while a shell's table, seated with its console streams,
 *          hands out three first — the shape every Unix program grew up
 *          with, arrived at honestly. adopt seats an open another table
 *          already owns, which is how a forked child inherits its
 *          parent's cursor. Each bitmap-sized chunk owns occupancy; every stored
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
    [[nodiscard]] base::Result<unsigned long> alloc(const FsOps* ops, void* backend, FsNode node,
                                                    OpenOptions options = {});

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
    [[nodiscard]] base::Result<unsigned long> adopt(const base::Shared<OpenFile>& file,
                                                    unsigned long limit = kFileTableMax);

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
     *                nothing is held, kTooManyOpenFiles when no seat at or
     *                above the floor is free.
     * @note          The limit is exclusive; a lowered resource limit
     *                constrains new descriptors without invalidating old ones.
     * @since         0.9.0
     * @ingroup       kernel_fs
     */
    [[nodiscard]] base::Result<unsigned long> adopt_from(const base::Shared<OpenFile>& file,
                                                         unsigned long floor, bool cloexec = false,
                                                         unsigned long limit = kFileTableMax);

    /**
     * @brief         Acquires an owning hold for reading and advancing.
     *
     * @param[in]     descriptor   The descriptor number.
     * @return        An owning hold, or kInvalidArgument (out of range),
     *                kNotFound (slot is free).
     * @since         0.1.0
     * @ingroup       kernel_fs
     */
    [[nodiscard]] base::Result<base::Shared<OpenFile>> get(unsigned long descriptor);

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

    /** @brief Copies descriptor ownership and close-on-exec bits in one locked snapshot. */
    [[nodiscard]] base::Shared<FileTable> clone();

    /**
     * @brief Atomically replaces a descriptor with another live descriptor's open.
     * @return The destination descriptor, or a lookup error without changing it.
     */
    [[nodiscard]] base::Result<unsigned long> duplicate_at(unsigned long source,
                                                           unsigned long destination,
                                                           unsigned long limit = kFileTableMax);

    /** @brief Descriptor span covered by the current dynamically grown table. */
    [[nodiscard]] unsigned long capacity() const;

private:
    struct TablePolicy {
        struct State {
            const FileTable&   table;
            unsigned long long irq = 0;
        };
        static void enter(State& state);
        static void exit(State& state);
    };
    using TableGuard = base::ScopedGuard<TablePolicy>;
    [[nodiscard]] base::Result<base::Shared<OpenFile>> shared_locked(unsigned long descriptor);
    [[nodiscard]] base::Shared<OpenFile>               detach_locked(unsigned long descriptor);
    mutable base::SpinMutex                            gate_;
    struct Chunk;
    [[nodiscard]] Chunk*                      chunk_locked(unsigned long descriptor) const;
    [[nodiscard]] Chunk*                      ensure_chunk_locked(unsigned long descriptor);
    [[nodiscard]] base::Result<unsigned long> vacant_locked(unsigned long floor,
                                                            unsigned long limit) const;
    base::Own<Chunk*[]>                       chunks_;
    unsigned long                             chunk_count_ = 0;
    unsigned long                             next_fd_     = 0;
};

}  // namespace cinux::fs
