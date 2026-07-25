/**
 * @file kernel/syscall/sys_mmap.cpp
 * @brief sys_mmap handler implementation (F2-M2 batch 1)
 *
 * Anonymous mmap: picks a free virtual range (or honours MAP_FIXED), records a
 * VMA in the current task's address space, and returns the base address.  No
 * physical pages are allocated here -- demand paging serves the first access.
 * File-backed mappings (fd != 0 without MAP_ANONYMOUS) are deferred to batch 4.
 *
 * Namespace: cinux::syscall
 */

#include "kernel/syscall/sys_mmap.hpp"

#include "kernel/arch/x86_64/memory_layout.hpp"
#include "kernel/arch/x86_64/paging_config.hpp"
#include "kernel/arch/x86_64/usermode.hpp"
#include "kernel/errno.hpp"
#include "kernel/fs/file.hpp"
#include "kernel/fs/inode.hpp"
#include "kernel/fs/vfs_mount.hpp"
#include "kernel/lib/aslr.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/mm/address_space.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/vma.hpp"
#include "kernel/proc/process.hpp"
#include "kernel/proc/scheduler.hpp"

namespace cinux::syscall {

namespace {

constexpr uint64_t kPageSize = 4096;

constexpr uint64_t align_up(uint64_t v) {
    return (v + kPageSize - 1) & ~(kPageSize - 1);
}

constexpr bool page_aligned(uint64_t v) {
    return (v & (kPageSize - 1)) == 0;
}

bool fixed_range_ok(uint64_t addr, uint64_t length) {
    if (!page_aligned(addr) || addr == 0 || addr + length < addr) {
        return false;
    }
    const uint64_t last = addr + length - 1;
    return cinux::arch::is_user_vaddr(addr) && cinux::arch::is_user_vaddr(last) &&
           addr + length <= cinux::arch::USER_STACK_TOP;
}

/// Translate POSIX prot/flags into the kernel VmaFlags (anonymous mappings).
cinux::mm::VmaFlags to_vma_flags(uint64_t prot, uint64_t flags) {
    cinux::mm::VmaFlags v = cinux::mm::VmaFlags::None;
    if ((flags & MAP_ANONYMOUS) != 0) {
        v |= cinux::mm::VmaFlags::Anonymous;
    }
    if (prot & PROT_READ) {
        v |= cinux::mm::VmaFlags::Read;
    }
    if (prot & PROT_WRITE) {
        v |= cinux::mm::VmaFlags::Write;
    }
    if (prot & PROT_EXEC) {
        v |= cinux::mm::VmaFlags::Exec;
    }
    if (flags & MAP_SHARED) {
        v |= cinux::mm::VmaFlags::Shared;
    }
    return v;
}

// Drop demand-paged phys + PTEs for [addr, addr+len). @p device_unmap skips
// pte_count_dec_and_test (IoPhys pages aren't PMM-managed).
void teardown_range_pages(cinux::mm::AddressSpace& as, uint64_t addr, uint64_t len,
                          bool device_unmap) {
    for (uint64_t v = addr; v < addr + len; v += kPageSize) {
        const uint64_t phys = as.translate(v);
        if (phys == 0) {
            continue;
        }
        as.unmap(v);
        if (!device_unmap) {
            cinux::mm::g_pmm.pte_count_dec_and_test(phys);
        }
    }
}

}  // namespace

int64_t sys_mmap(uint64_t addr, uint64_t length, uint64_t prot, uint64_t flags, uint64_t fd,
                 uint64_t offset) {
    if (length == 0) {
        return -kEinval;
    }

    // Resolve the backing: anonymous mappings have none; file mappings take
    // fd -> inode (basic -- demand-reading file contents arrives in M4).
    cinux::fs::Inode* backing_inode = nullptr;
    if ((flags & MAP_ANONYMOUS) == 0) {
        auto* file = cinux::fs::current_fd_table().get(static_cast<int>(fd));
        if (file == nullptr) {
            return -kEbadf;
        }
        backing_inode = file->inode;
        // File mappings need a page-aligned offset: the page cache (F2-M4) keys
        // whole pages by offset and demand paging maps them verbatim.
        if (offset % kPageSize != 0) {
            return -kEinval;
        }
    } else {
        // Anonymous mappings require exactly one of MAP_SHARED / MAP_PRIVATE.
        const bool shared = (flags & MAP_SHARED) != 0;
        const bool priv   = (flags & MAP_PRIVATE) != 0;
        if (shared == priv) {
            return -kEinval;
        }
    }

    auto* task = cinux::proc::Scheduler::current();
    if (task == nullptr || task->addr_space == nullptr) {
        return -kEnomem;
    }

    const uint64_t aligned_len = align_up(length);

    // F-GUI-USERSPACE batch 1: device mmap probe.  For a fd-backed mapping,
    // ask the inode's mmap hook whether it is device memory (e.g. /dev/fb0
    // returns the framebuffer's VBE physical base).  A device mmap binds the
    // VMA to that fixed physical range (IoPhys, uncached, not PMM-managed);
    // NotImplemented means "ordinary file" -> take the page-cache path below.
    bool     device_mmap = false;
    uint64_t device_phys = 0;
    if (backing_inode != nullptr && backing_inode->ops != nullptr) {
        auto mp = backing_inode->ops->mmap(backing_inode, offset, aligned_len);
        if (mp.ok()) {
            device_mmap = true;
            device_phys = mp.value();
        } else if (mp.error() != cinux::lib::Error::NotImplemented) {
            return -to_errno(mp.error());
        }
    }

    uint64_t map_addr = 0;
    if ((flags & MAP_FIXED) != 0) {
        // Honour the exact user address. MAP_FIXED may intentionally replace a
        // low PIE/heap VMA, so do not confine it to the high mmap arena.
        if (!fixed_range_ok(addr, aligned_len)) {
            return -kEinval;
        }
        map_addr = addr;
        // E: teardown the old mapping's pages before the new map overwrites the
        // PTEs unconditionally -- the old phys's pte_count must drop here.
        {
            const cinux::mm::VMA* old = task->addr_space->vmas().find(map_addr);
            const bool old_device = old != nullptr &&
                cinux::mm::has_flag(old->flags, cinux::mm::VmaFlags::IoPhys);
            teardown_range_pages(*task->addr_space, map_addr, aligned_len, old_device);
        }
        static_cast<void>(task->addr_space->vmas().remove(map_addr, map_addr + aligned_len));
    } else {
        // F9 batch 8 (ASLR): jitter the first-fit hint so each process's
        // mappings start at an unpredictable address. The window bounds stay
        // fixed; MAP_FIXED (above) still honours the caller's exact address.
        const uint64_t hint = cinux::arch::USER_MMAP_BASE + cinux::lib::aslr_mmap_offset();
        auto           area = task->addr_space->vmas().find_free_area(hint, aligned_len);
        if (!area.ok()) {
            return -to_errno(area.error());
        }
        map_addr = area.value();
        if (map_addr + aligned_len > cinux::arch::USER_MMAP_END ||
            map_addr + aligned_len < map_addr) {
            return -kEnomem;
        }
    }

    // Device mmap overrides the anonymous/file-backed flags: it is its own VMA
    // kind (IoPhys), never page-cache-backed.
    cinux::mm::VmaFlags vflags = to_vma_flags(prot, flags);
    if (device_mmap) {
        vflags |= cinux::mm::VmaFlags::IoPhys;
    }
    auto ir = task->addr_space->vmas().insert(map_addr, map_addr + aligned_len, vflags);
    if (!ir.ok()) {
        return -to_errno(ir.error());
    }

    cinux::mm::VMA* v = task->addr_space->vmas().find(map_addr);
    if (v != nullptr) {
        if (device_mmap) {
            // Bind the VMA to device memory.  No inode ref: device memory is
            // not page cache and the VMA is not file-backed in the PageCache
            // sense; phys_base is the per-page physical source for the fault
            // handler.  backing/file_offset stay null/0.
            v->phys_base = device_phys;
        } else if (backing_inode != nullptr) {
            // Attach the file backing (if any) to the freshly recorded VMA.
            // Contents are demand-read via the Page Cache in M4; here we only
            // remember the inode.  Take an inode reference so the backing stays
            // alive for the lifetime of the mapping (the fd that mmap'd it may
            // close, but the VMA persists); the VMA store drops the ref when
            // the node is freed (clear/split/remove in vma.cpp).
            v->backing     = backing_inode;
            v->file_offset = offset;
            cinux::fs::inode_ref(backing_inode);
        }
    }

    return static_cast<int64_t>(map_addr);
}

int64_t sys_munmap(uint64_t addr, uint64_t length, uint64_t, uint64_t, uint64_t, uint64_t) {
    if (length == 0 || !page_aligned(addr)) {
        return -kEinval;
    }
    const uint64_t aligned_len = align_up(length);
    if (addr + aligned_len < addr) {
        return -kEinval;  // overflow
    }

    auto* task = cinux::proc::Scheduler::current();
    if (task == nullptr || task->addr_space == nullptr) {
        return -kEinval;
    }

    // F-GUI-USERSPACE batch 1: a device (IoPhys) mapping's pages are device
    // memory, not PMM-managed, so munmap only drops their PTEs -- never
    // pte_count_dec_and_test (which would hand the framebuffer physical page
    // back to the PMM and cause mayhem on the next allocation).  We assume the
    // range lies in a single VMA (the normal case: munmap the whole mapping);
    // partial split of an IoPhys VMA is a follow-up.
    const cinux::mm::VMA* vma = task->addr_space->vmas().find(addr);
    const bool            device_unmap =
        vma != nullptr && cinux::mm::has_flag(vma->flags, cinux::mm::VmaFlags::IoPhys);

    // Free any demand-paged physical pages in the range and drop their PTEs.
    // These are user pages, not the higher-half direct map, so unmapping is
    // safe (cf. GOTCHA #7 -- never unmap phys+KERNEL_VMA).
    teardown_range_pages(*task->addr_space, addr, aligned_len, device_unmap);

    // Remove the VMA range (splits a VMA when only its interior is taken).
    auto r = task->addr_space->vmas().remove(addr, addr + aligned_len);
    if (!r.ok()) {
        return -to_errno(r.error());
    }
    return 0;
}

int64_t sys_mprotect(uint64_t addr, uint64_t length, uint64_t prot, uint64_t, uint64_t, uint64_t) {
    if (length == 0 || !page_aligned(addr)) {
        return -kEinval;
    }
    const uint64_t aligned_len = align_up(length);
    if (addr + aligned_len < addr) {
        return -kEinval;
    }

    auto* task = cinux::proc::Scheduler::current();
    if (task == nullptr || task->addr_space == nullptr) {
        return -kEinval;
    }

    // The start of the range must be currently mapped.
    cinux::mm::VMA* existing = task->addr_space->vmas().find(addr);
    if (existing == nullptr) {
        return -kEnomem;  // POSIX: ENOMEM if any page of the range is unmapped
    }

    // Preserve the non-protection attributes; replace R/W/X from @p prot.
    using cinux::mm::VmaFlags;
    const VmaFlags    base        = existing->flags & (VmaFlags::Anonymous | VmaFlags::Shared |
                                                       VmaFlags::Stack | VmaFlags::Heap);
    cinux::fs::Inode* backing     = existing->backing;
    uint64_t          file_offset = existing->file_offset + (addr - existing->start);
    if (backing != nullptr) {
        cinux::fs::inode_ref(backing);
    }

    VmaFlags vma = base;
    if ((prot & PROT_READ) != 0) {
        vma |= VmaFlags::Read;
    }
    if ((prot & PROT_WRITE) != 0) {
        vma |= VmaFlags::Write;
    }
    if ((prot & PROT_EXEC) != 0) {
        vma |= VmaFlags::Exec;
    }

    // Re-record the range with the new flags (splits when partially covered).
    static_cast<void>(task->addr_space->vmas().remove(addr, addr + aligned_len));
    auto ir = task->addr_space->vmas().insert(addr, addr + aligned_len, vma);
    if (!ir.ok()) {
        if (backing != nullptr) {
            cinux::fs::inode_unref(backing);
        }
        return -to_errno(ir.error());
    }
    if (backing != nullptr) {
        if (cinux::mm::VMA* updated = task->addr_space->vmas().find(addr)) {
            updated->backing     = backing;
            updated->file_offset = file_offset;
        } else {
            cinux::fs::inode_unref(backing);
        }
    }

    // Re-issue PTE permissions for any already-mapped pages (map() overwrites).
    uint64_t pte_flags = cinux::arch::FLAG_PRESENT | cinux::arch::FLAG_USER;
    if ((prot & PROT_WRITE) != 0) {
        pte_flags |= cinux::arch::FLAG_WRITABLE;
    }
    // F9 batch 2: NXE is on -- non-executable mappings get the NX bit (bit 63
    // is valid now; was a reserved-bit #PF before EFER.NXE). PROT_EXEC stays
    // executable; everything else is NX (W^X).
    if ((prot & PROT_EXEC) == 0) {
        pte_flags |= cinux::arch::FLAG_NX;
    }
    for (uint64_t v = addr; v < addr + aligned_len; v += kPageSize) {
        const uint64_t phys = task->addr_space->translate(v);
        if (phys != 0) {
            task->addr_space->map(v, phys, pte_flags);
        }
    }
    return 0;
}

}  // namespace cinux::syscall
