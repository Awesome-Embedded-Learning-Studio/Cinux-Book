/**
 * @file kernel/mm/vma.hpp
 * @brief VMA (Virtual Memory Area) bookkeeping for per-process address spaces
 *
 * A VMA records a contiguous half-open virtual range [start, end) with access
 * flags and, for file mappings (M2+), an optional backing inode.  An IVMAStore
 * holds the set of VMAs for one AddressSpace and answers the questions the rest
 * of the VM subsystem asks:
 *
 *   - find(addr)         -- which VMA (if any) owns this faulting address?
 *                           (page-fault validation, batch 4)
 *   - find_free_area()   -- where can a new mapping of length N go?  (mmap, M2)
 *   - insert()/remove()  -- track and untrack ranges, merging/splitting so the
 *                           store stays a minimal sorted list.
 *
 * IVMAStore is abstract so the backend can be swapped (a red-black tree is the
 * natural future upgrade) without touching callers.  This file ships
 * LinkedListVMAStore, an intrusive doubly-linked list kept sorted by start.
 *
 * Ownership: the store OWNS every VMA node it holds.  insert() allocates each
 * node with global operator new (Heap::alloc); the destructor and remove()
 * free nodes.  Pointers returned by find()/first()/next() are borrowed and must
 * not be deleted by the caller.
 *
 * Failure model: invalid arguments and overlapping inserts are reported via
 * cinux::lib::ErrorOr (A.6).  Heap exhaustion is not modelled as a recoverable
 * error -- operator new returns null and construction traps on OOM, the same
 * convention used elsewhere in the kernel (e.g. `new AddressSpace()`); the heap
 * auto-expands, so this is not expected in practice.
 *
 * Synchronization: LinkedListVMAStore is NOT thread-safe.  The owning
 * AddressSpace serialises access with its own lock (batch 2).  Single-threaded
 * unit tests need no locking.
 *
 * Namespace: cinux::mm
 */

#pragma once

#include <cinux/expected.hpp>
#include <cstddef>
#include <cstdint>

namespace cinux::fs {
class InodeOps;  // forward declaration; file backings are filled in M2 (mmap)
}

namespace cinux::mm {

/// Access flags for a VMA, mirroring the POSIX mmap prot/flags intent.
enum class VmaFlags : uint64_t {
    None      = 0,
    Read      = 1 << 0,  ///< PROT_READ
    Write     = 1 << 1,  ///< PROT_WRITE
    Exec      = 1 << 2,  ///< PROT_EXEC
    Shared    = 1 << 3,  ///< MAP_SHARED (vs MAP_PRIVATE)
    Anonymous = 1 << 4,  ///< MAP_ANONYMOUS (no backing file)
    Stack     = 1 << 5,  ///< grows-down stack region (auto-expand on PF, batch 4)
    Heap      = 1 << 6,  ///< brk-managed heap region (M3)
};

constexpr VmaFlags operator|(VmaFlags a, VmaFlags b) noexcept {
    return static_cast<VmaFlags>(static_cast<uint64_t>(a) | static_cast<uint64_t>(b));
}

constexpr VmaFlags& operator|=(VmaFlags& a, VmaFlags b) noexcept {
    a = a | b;
    return a;
}

/// @brief True when @p value has every bit of @p bit set.
constexpr bool has_flag(VmaFlags value, VmaFlags bit) noexcept {
    return (static_cast<uint64_t>(value) & static_cast<uint64_t>(bit)) ==
           static_cast<uint64_t>(bit);
}

/**
 * @brief One contiguous virtual memory region [start, end).
 *
 * Nodes are intrusive (prev/next) so an IVMAStore can link them without a side
 * table.  They are heap-allocated and owned by the store.
 */
struct VMA {
    uint64_t             start{};  ///< Range start (page-aligned), inclusive
    uint64_t             end{};    ///< Range end (page-aligned), exclusive
    VmaFlags             flags{VmaFlags::None};
    cinux::fs::InodeOps* backing{nullptr};  ///< File backend (M2 mmap); null when anonymous
    uint64_t             file_offset{0};    ///< Offset into the backing inode (M2)
    VMA*                 prev{nullptr};     ///< Intrusive list link
    VMA*                 next{nullptr};
};

/**
 * @brief Abstract set of VMAs for one address space
 *
 * Implementations keep the set sorted by start address and coalesce adjacent
 * ranges with identical flags.  Mutating operations report logical failures
 * (bad range, overlap) via cinux::lib::ErrorOr (A.6); queries return raw
 * borrowed pointers, using nullptr for the normal "not found" case.
 */
class IVMAStore {
public:
    virtual ~IVMAStore() = default;

    /// Track [start, end) with @p flags, merging with adjacent same-flags
    /// neighbours.  @return Error::InvalidArgument (bad/unaligned range) or
    /// Error::AlreadyExists (overlaps an existing VMA).
    virtual cinux::lib::ErrorOr<void> insert(uint64_t start, uint64_t end, VmaFlags flags) = 0;

    /// Untrack the intersection of every VMA with [start, end), splitting a VMA
    /// when only its middle is removed.  @return Error::InvalidArgument (bad
    /// range).
    virtual cinux::lib::ErrorOr<void> remove(uint64_t start, uint64_t end) = 0;

    /// VMA that contains @p addr, or nullptr if none.
    virtual VMA* find(uint64_t addr) = 0;

    /// Lowest page-aligned address >= @p hint backed by a gap of @p length.
    /// @return Error::InvalidArgument when @p length is 0.
    virtual cinux::lib::ErrorOr<uint64_t> find_free_area(uint64_t hint, uint64_t length) = 0;

    /// First VMA in address order (for fork/execve traversal), or nullptr.
    virtual VMA* first() = 0;

    /// VMA following @p cur in address order, or nullptr.
    virtual VMA* next(VMA* cur) = 0;

    /// Number of VMA nodes currently held.
    virtual std::size_t count() const = 0;
};

/**
 * @brief Sorted intrusive doubly-linked-list backend for IVMAStore
 *
 * O(n) per operation -- fine for a teaching kernel's modest VMA count; the
 * interface lets a future O(log n) tree backend drop in unchanged.
 */
class LinkedListVMAStore : public IVMAStore {
public:
    LinkedListVMAStore() = default;
    ~LinkedListVMAStore() override;

    LinkedListVMAStore(const LinkedListVMAStore&)            = delete;
    LinkedListVMAStore& operator=(const LinkedListVMAStore&) = delete;

    // Move transfers ownership of every node; the source is left empty so its
    // destructor frees nothing.  Required because AddressSpace (which holds a
    // store by value) is itself move-only.
    LinkedListVMAStore(LinkedListVMAStore&& other) noexcept;
    LinkedListVMAStore& operator=(LinkedListVMAStore&& other) noexcept;

    cinux::lib::ErrorOr<void>     insert(uint64_t start, uint64_t end, VmaFlags flags) override;
    cinux::lib::ErrorOr<void>     remove(uint64_t start, uint64_t end) override;
    VMA*                          find(uint64_t addr) override;
    cinux::lib::ErrorOr<uint64_t> find_free_area(uint64_t hint, uint64_t length) override;
    VMA*                          first() override { return head_; }
    VMA*                          next(VMA* cur) override { return cur ? cur->next : nullptr; }
    std::size_t                   count() const override { return count_; }

private:
    /// Free every node (destructor helper).
    void clear();

    VMA*        head_{nullptr};
    std::size_t count_{0};
};

}  // namespace cinux::mm
