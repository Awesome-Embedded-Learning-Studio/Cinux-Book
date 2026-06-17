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
#include "kernel/errno.hpp"
#include "kernel/fs/file.hpp"
#include "kernel/fs/inode.hpp"
#include "kernel/fs/vfs_mount.hpp"
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

    uint64_t map_addr = 0;
    if ((flags & MAP_FIXED) != 0) {
        // Honour the requested address, validating alignment + mmap window.
        if (!page_aligned(addr) || addr < cinux::arch::USER_MMAP_BASE ||
            addr + aligned_len > cinux::arch::USER_MMAP_END || addr + aligned_len < addr) {
            return -kEinval;
        }
        map_addr = addr;
        // Drop any prior VMA in this range (physical pages freed in batch 2's
        // munmap; here we only fix the bookkeeping).
        (void)task->addr_space->vmas().remove(map_addr, map_addr + aligned_len);
    } else {
        auto area =
            task->addr_space->vmas().find_free_area(cinux::arch::USER_MMAP_BASE, aligned_len);
        if (!area.ok()) {
            return -to_errno(area.error());
        }
        map_addr = area.value();
        if (map_addr + aligned_len > cinux::arch::USER_MMAP_END ||
            map_addr + aligned_len < map_addr) {
            return -kEnomem;
        }
    }

    auto ir = task->addr_space->vmas().insert(map_addr, map_addr + aligned_len,
                                              to_vma_flags(prot, flags));
    if (!ir.ok()) {
        return -to_errno(ir.error());
    }

    // Attach the file backing (if any) to the freshly recorded VMA.  Contents
    // are demand-read via the Page Cache in M4; here we only remember the inode.
    if (backing_inode != nullptr) {
        cinux::mm::VMA* v = task->addr_space->vmas().find(map_addr);
        if (v != nullptr) {
            v->backing     = backing_inode;
            v->file_offset = offset;
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

    // Free any demand-paged physical pages in the range and drop their PTEs.
    // These are user pages, not the higher-half direct map, so unmapping is
    // safe (cf. GOTCHA #7 -- never unmap phys+KERNEL_VMA).
    for (uint64_t v = addr; v < addr + aligned_len; v += kPageSize) {
        const uint64_t phys = task->addr_space->translate(v);
        if (phys != 0) {
            task->addr_space->unmap(v);
            cinux::mm::g_pmm.free_page(phys);
        }
    }

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
    const VmaFlags base = existing->flags & (VmaFlags::Anonymous | VmaFlags::Shared |
                                             VmaFlags::Stack | VmaFlags::Heap);
    VmaFlags       vma  = base;
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
    (void)task->addr_space->vmas().remove(addr, addr + aligned_len);
    auto ir = task->addr_space->vmas().insert(addr, addr + aligned_len, vma);
    if (!ir.ok()) {
        return -to_errno(ir.error());
    }

    // Re-issue PTE permissions for any already-mapped pages (map() overwrites).
    uint64_t pte_flags = cinux::arch::FLAG_PRESENT | cinux::arch::FLAG_USER;
    if ((prot & PROT_WRITE) != 0) {
        pte_flags |= cinux::arch::FLAG_WRITABLE;
    }
    // NX deferred until EFER.NXE is enabled (F9 NX/SMEP/SMAP): bit 63 is
    // reserved now and would cause a reserved-bit #PF on access.
    for (uint64_t v = addr; v < addr + aligned_len; v += kPageSize) {
        const uint64_t phys = task->addr_space->translate(v);
        if (phys != 0) {
            task->addr_space->map(v, phys, pte_flags);
        }
    }
    return 0;
}

}  // namespace cinux::syscall
