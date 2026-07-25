/**
 * @file kernel/ipc/shm.hpp
 * @brief SysV shared memory -- ShmSegment + ShmRegistry (F8-M4)
 *
 * Two or more processes share a physical page range: shmget() allocates a
 * contiguous run of physical pages and registers it under an int key; shmat()
 * maps that run into the caller's address space; shmdt() tears the mapping
 * down; shmctl(IPC_RMID) marks the segment for destruction (the pages are freed
 * once the last attachment goes away).
 *
 * This file is the IPC-namespace table layer only -- it owns the segment
 * bookkeeping (key, size, attach count, removal state) but NOT the physical
 * pages.  sys_shm.cpp allocates/frees the frames (it needs the PMM, which is a
 * kernel-only dependency), mirroring the split between FifoOps (filesystem
 * name) and Pipe (buffer) in F8-M2.  Pure logic, no kprintf / no I/O, so the TU
 * links cleanly into host unit tests like fifo.cpp.
 *
 * pte_count lifecycle (the part that matters here): alloc_pages() sets each
 * page's pte_count to 1 (the segment's own reference); each shmat does +1 per
 * page; each address-space teardown / shmdt does -1.  So teardown never drives
 * the count to 0 while the segment exists -- only IPC_RMID's explicit free
 * returns the pages.  sys_shm.cpp drives pte_count_inc / pte_count_dec_and_test.
 *
 * shmid = the table index (0 .. SHM_REGISTRY_MAX-1).  Linux embeds a sequence
 * number to avoid rapid reuse; a teaching kernel's fixed table is fine and
 * matches FifoRegistry's index-as-handle model.
 *
 * Namespace: cinux::ipc
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <cinux/expected.hpp>

#include "kernel/proc/sync.hpp"  // Spinlock

namespace cinux::ipc {

// ============================================================
// Constants (Linux x86_64 values)
// ============================================================

/// shmget key reserved value: always create a new, unnamed segment.
static constexpr int kIpcPrivate = 0;

/// shmflg creation bits (low bits carry the permission mode).
static constexpr uint64_t kIpcCreat  = 0x200;   ///< 00001000 octal: create if absent
static constexpr uint64_t kIpcExcl   = 0x400;   ///< 00002000 octal: fail if exists
static constexpr uint64_t kShmRdonly = 0x1000;  ///< shmat: map read-only
static constexpr uint64_t kShmRnd =
    0x2000;  ///< shmat: round addr to SHMLBA (treated as page-align)

/// shmctl sub-commands (the cmd argument).
static constexpr int kIpcRmid = 0;  ///< remove the segment
static constexpr int kIpcSet  = 1;  ///< set attributes (deferred)
static constexpr int kIpcStat = 2;  ///< copy a shmid_ds to user
static constexpr int kIpcInfo = 3;  ///< deferred

/// Upper bound on a single segment's page count.  A sanity cap so a runaway
/// shmget does not drain the PMM; the PMM still returns 0 on real OOM.
static constexpr uint64_t kShmMaxPages = 1024;

/// Maximum number of concurrently live segments.
static constexpr uint32_t kShmRegistryMax = 16;

// ============================================================
// shmid_ds -- IPC_STAT output (reduced kernel-internal shape)
// ============================================================

/**
 * @brief Segment descriptor returned by IPC_STAT
 *
 * A trimmed subset of Linux's struct shmid_ds: the fields a teaching kernel
 * actually tracks.  Future musl/glibc interop may widen this toward the full
 * ipc_perm + timestamp layout; today only the size / attach count / pids are
 * meaningful and the rest is omitted.
 */
struct shmid_ds {
    uint64_t shm_segsz{0};   ///< segment size in bytes
    uint32_t shm_cpid{0};    ///< creator pid (tid)
    uint32_t shm_lpid{0};    ///< pid of last shmat / shmdt
    uint32_t shm_nattch{0};  ///< current number of attachments
    uint16_t shm_mode{0};    ///< permission bits (low 9)
};

// ============================================================
// ShmSegment -- one shared memory segment
// ============================================================

/**
 * @brief Bookkeeping for one SysV shared memory segment
 *
 * phys_base / page_count are filled by the syscall layer after it allocates the
 * frames; the registry only stores them so shmat/shmdt can find the backing.
 */
struct ShmSegment {
    bool     used{false};                ///< slot occupied
    int      key{0};                     ///< IPC_PRIVATE (0) or user-supplied key
    uint64_t phys_base{0};               ///< physical base of the page run
    uint64_t page_count{0};              ///< number of 4 KB pages mapped
    uint64_t size{0};                    ///< requested size in bytes (for IPC_STAT)
    uint16_t mode{0666};                 ///< permission bits (low 9)
    uint32_t nattach{0};                 ///< live shmat count
    uint32_t cpid{0};                    ///< creator tid
    uint32_t lpid{0};                    ///< last shmat / shmdt tid
    bool     marked_for_removal{false};  ///< IPC_RMID set; freed when nattach hits 0
};

// ============================================================
// ShmRegistry -- fixed-size key -> segment table
// ============================================================

/**
 * @brief Fixed-size in-memory registry of SysV shared memory segments
 *
 * Addresses segments by int key (shmget) and by table index / shmid (shmat /
 * shmctl / shmdt).  A fixed table keeps the kernel off <map>/<string>, the same
 * rationale as FifoRegistry.  The registry owns table state only; the syscall
 * layer owns the physical page lifecycle (it alloc_pages at shmget and
 * free_pages when the registry reports a segment is finally reclaimable).
 */
class ShmRegistry {
public:
    /// Process-wide singleton (one SysV SHM namespace).
    static ShmRegistry& instance();

    /// Look up an existing segment by @p key.  IPC_PRIVATE never matches.
    /// @return shmid or NotFound.
    cinux::lib::ErrorOr<int> find_by_key(int key) const;

    /**
     * @brief Register a freshly-allocated segment.
     *
     * Finds a free slot, fills it with the given metadata, and returns the
     * shmid (table index).  The caller (syscall layer) must have already
     * allocated @p phys_base; the registry never touches the PMM.
     *
     * @return shmid, or OutOfMemory when the table is full.
     */
    cinux::lib::ErrorOr<int> add(int key, uint64_t size, uint64_t phys_base, uint64_t page_count,
                                 uint16_t mode);

    /// Borrow a segment by shmid (range + used checked).  nullptr if invalid.
    /// Caller-supplied exclusion is assumed (shmat holds the segment via the
    /// snapshot returned by attach(), so the pointer is only used for a stable
    /// post-snapshot read of phys_base / page_count).
    ShmSegment* segment(int shmid);

    /**
     * @brief Attach: snapshot a segment and bump its attach count atomically.
     *
     * Refuses a segment that is out of range, unused, or already marked for
     * removal (the syscall maps all of those to -EINVAL; Linux further splits
     * the marked-for-removal case into -EIDRM, deferred here for simplicity).
     *
     * @param shmid   Segment to attach.
     * @param tid     Attaching tid (recorded as lpid).
     * @return Snapshot of the segment (phys_base / page_count / mode), or
     *         NotFound if the id is invalid or being torn down.
     */
    cinux::lib::ErrorOr<ShmSegment> attach(int shmid, uint32_t tid);

    /**
     * @brief Detach: drop one attach reference.
     *
     * Decrements nattach and records @p tid as lpid.  If that brought a
     * marked-for-removal segment to 0, the slot is cleared and the physical
     * pages are returned for the caller to free.
     *
     * @return phys_base to free now (with count in @p out_count), or 0 if the
     *         segment is still live (or the shmid was invalid -- a benign no-op
     *         matching shmdt of a stale address).
     */
    uint64_t detach(int shmid, uint32_t tid, uint64_t* out_count);

    /**
     * @brief IPC_RMID: mark a segment for removal.
     *
     * If nattach == 0 the slot is cleared immediately and the phys base is
     * returned for freeing; otherwise the segment lingers, marked, and is freed
     * on the last detach.
     *
     * @return phys_base to free now (count in @p out_count), or 0 if it lingers.
     */
    uint64_t mark_removal(int shmid, uint64_t* out_count);

    /// Fill @p out with the segment descriptor (IPC_STAT).  NotFound if invalid.
    cinux::lib::ErrorOr<void> stat(int shmid, shmid_ds* out) const;

    /// Find the shmid whose phys_base matches @p phys_base (shmdt resolution).
    /// NotFound if none.
    cinux::lib::ErrorOr<int> find_by_phys(uint64_t phys_base) const;

private:
    ShmRegistry() = default;

    struct Entry {
        bool       used{false};
        ShmSegment seg;
    };

    Entry                         entries_[kShmRegistryMax];
    mutable cinux::proc::Spinlock lock_;

    /// Index of @p key among used entries, or -1.  Caller holds lock_.
    int find_key_locked(int key) const;
    /// Index of @p phys_base among used entries, or -1.  Caller holds lock_.
    int find_phys_locked(uint64_t phys_base) const;
    /// First free slot index, or -1.  Caller holds lock_.
    int find_free_locked() const;
};

}  // namespace cinux::ipc
