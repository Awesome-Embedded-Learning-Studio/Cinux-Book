/**
 * @file kernel/mm/vma.cpp
 * @brief LinkedListVMAStore implementation
 *
 * Keeps an intrusive doubly-linked list of VMA nodes sorted by start address.
 * insert() merges with adjacent same-flags neighbours; remove() splits a node
 * when only its interior is taken.  See vma.hpp for the ownership and failure
 * model.
 *
 * Namespace: cinux::mm
 */

#include "kernel/mm/vma.hpp"

#include "kernel/fs/file.hpp"  // inode_ref/inode_unref (VMA holds an inode ref)

namespace cinux::mm {

namespace {

/// Page size used to validate range alignment (VMA granularity is one page).
constexpr uint64_t kPageSize = 4096;

/// True when @p v is a multiple of the page size.
constexpr bool page_aligned(uint64_t v) {
    return (v & (kPageSize - 1)) == 0;
}

/// Round @p v up to the next page boundary.
constexpr uint64_t align_up_page(uint64_t v) {
    return (v + kPageSize - 1) & ~(kPageSize - 1);
}

/// Drop the inode reference a VMA holds for its file backing (if any).  Called
/// before a node is freed so the backing inode stays alive exactly as long as a
/// VMA maps it -- without this, the ext2 inode-cache slot can be evicted and
/// reused for a different file while a VMA still points at it (the ld crash:
/// a stale backing served libc.so-script zero pages to a mapped library .text).
void release_backing(VMA* v) {
    if (v != nullptr && v->backing != nullptr) {
        cinux::fs::inode_unref(v->backing);
    }
}

/// Allocate and initialise an unlinked VMA node.  Heap exhaustion traps via
/// operator new (see vma.hpp failure model).
VMA* make_node(uint64_t start, uint64_t end, VmaFlags flags) {
    auto* node        = new VMA;
    node->start       = start;
    node->end         = end;
    node->flags       = flags;
    node->backing     = nullptr;
    node->file_offset = 0;
    node->prev        = nullptr;
    node->next        = nullptr;
    return node;
}

}  // namespace

LinkedListVMAStore::~LinkedListVMAStore() {
    clear();
}

void LinkedListVMAStore::clear() {
    VMA* cur = head_;
    while (cur != nullptr) {
        VMA* nxt = cur->next;
        release_backing(cur);
        delete cur;
        cur = nxt;
    }
    head_  = nullptr;
    count_ = 0;
}

LinkedListVMAStore::LinkedListVMAStore(LinkedListVMAStore&& other) noexcept
    : head_(other.head_), count_(other.count_) {
    other.head_  = nullptr;
    other.count_ = 0;
}

LinkedListVMAStore& LinkedListVMAStore::operator=(LinkedListVMAStore&& other) noexcept {
    if (this != &other) {
        clear();
        head_        = other.head_;
        count_       = other.count_;
        other.head_  = nullptr;
        other.count_ = 0;
    }
    return *this;
}

cinux::lib::ErrorOr<void> LinkedListVMAStore::insert(uint64_t start, uint64_t end, VmaFlags flags) {
    if (start >= end || !page_aligned(start) || !page_aligned(end)) {
        return cinux::lib::Error::InvalidArgument;
    }

    // Walk to the first node whose start >= `start`; `prev` trails it.  Because
    // the list is sorted, only prev/cur can overlap or merge with the new range.
    VMA* prev = nullptr;
    VMA* cur  = head_;
    while (cur != nullptr && cur->start < start) {
        prev = cur;
        cur  = cur->next;
    }

    // Overlap: prev extends into the new range, or cur starts inside it.
    if (prev != nullptr && prev->end > start) {
        return cinux::lib::Error::AlreadyExists;
    }
    if (cur != nullptr && cur->start < end) {
        return cinux::lib::Error::AlreadyExists;
    }

    // Only merge plain unbacked VMAs. File-backed mappings carry per-node
    // file_offset/backing metadata; merging by flags alone would make one file
    // offset cover non-contiguous ELF segments.
    const bool merge_prev = (prev != nullptr) && (prev->end == start) && (prev->flags == flags) &&
                            prev->backing == nullptr;
    const bool merge_next =
        (cur != nullptr) && (cur->start == end) && (cur->flags == flags) && cur->backing == nullptr;

    if (merge_prev && merge_next) {
        // Bridge prev and cur into a single node.
        prev->end  = cur->end;
        prev->next = cur->next;
        if (cur->next != nullptr) {
            cur->next->prev = prev;
        }
        release_backing(cur);
        delete cur;
        --count_;
        return {};
    }
    if (merge_prev) {
        prev->end = end;
        return {};
    }
    if (merge_next) {
        cur->start = start;  // extend cur's left edge; position unchanged
        return {};
    }

    // No neighbour absorbs it: splice a fresh node between prev and cur.
    VMA* node  = make_node(start, end, flags);
    node->prev = prev;
    node->next = cur;
    if (cur != nullptr) {
        cur->prev = node;
    }
    if (prev != nullptr) {
        prev->next = node;
    } else {
        head_ = node;  // new lowest node
    }
    ++count_;
    return {};
}

cinux::lib::ErrorOr<void> LinkedListVMAStore::remove(uint64_t start, uint64_t end) {
    if (start >= end || !page_aligned(start) || !page_aligned(end)) {
        return cinux::lib::Error::InvalidArgument;
    }

    VMA* cur = head_;
    while (cur != nullptr) {
        VMA* nxt = cur->next;  // capture before cur may be deleted

        const bool no_overlap = (cur->end <= start) || (cur->start >= end);
        if (no_overlap) {
            cur = nxt;
            continue;
        }

        const bool keep_left  = cur->start < start;  // survivor [cur->start, start)
        const bool keep_right = cur->end > end;      // survivor [end, cur->end)

        if (keep_left && keep_right) {
            // Middle removed: shrink cur to the left part, splice in a fresh
            // node for the right part (same backing/flags as cur).  The right
            // survivor is a NEW node referencing the same backing, so it takes
            // its own inode_ref (cur keeps its ref for the left survivor).
            const uint64_t old_start = cur->start;
            VMA*           right     = make_node(end, cur->end, cur->flags);
            right->backing           = cur->backing;
            right->file_offset       = cur->file_offset + (end - old_start);
            if (right->backing != nullptr) {
                cinux::fs::inode_ref(right->backing);
            }
            cur->end    = start;
            right->prev = cur;
            right->next = cur->next;
            if (cur->next != nullptr) {
                cur->next->prev = right;
            }
            cur->next = right;
            ++count_;
        } else if (keep_left) {
            cur->end = start;  // trim right edge
        } else if (keep_right) {
            cur->file_offset += end - cur->start;
            cur->start = end;  // trim left edge
        } else {
            // Fully inside [start, end): unlink and free.
            if (cur->prev != nullptr) {
                cur->prev->next = cur->next;
            } else {
                head_ = cur->next;
            }
            if (cur->next != nullptr) {
                cur->next->prev = cur->prev;
            }
            release_backing(cur);
            delete cur;
            --count_;
        }
        cur = nxt;
    }
    return {};
}

VMA* LinkedListVMAStore::find(uint64_t addr) {
    for (VMA* cur = head_; cur != nullptr; cur = cur->next) {
        if (cur->start > addr) {
            break;  // sorted: no later node can contain addr
        }
        if (addr < cur->end) {
            return cur;  // cur->start <= addr < cur->end
        }
    }
    return nullptr;
}

cinux::lib::ErrorOr<uint64_t> LinkedListVMAStore::find_free_area(uint64_t hint, uint64_t length) {
    if (length == 0) {
        return cinux::lib::Error::InvalidArgument;
    }
    uint64_t search = page_aligned(hint) ? hint : align_up_page(hint);

    for (VMA* cur = head_; cur != nullptr; cur = cur->next) {
        if (cur->start > search) {
            const uint64_t gap = cur->start - search;
            if (gap >= length) {
                return search;  // gap before cur is big enough
            }
        }
        if (cur->end > search) {
            search = cur->end;  // advance past this node
        }
    }
    return search;  // unbounded tail: always fits (M2 adds an upper bound)
}

}  // namespace cinux::mm
