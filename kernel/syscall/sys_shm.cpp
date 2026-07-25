/**
 * @file kernel/syscall/sys_shm.cpp
 * @brief SysV shared memory syscall handlers (F8-M4)
 *
 * The syscall layer owns the physical-page plumbing the ShmRegistry avoids.
 * shmget allocates a contiguous run via the PMM; shmat maps it eagerly (the
 * pages already exist, so no demand-paging path is involved) and bumps each
 * page's pte_count so the segment's frames survive address-space teardown; shmdt
 * reverses the mapping and lets the registry free the pages once the last
 * attachment is gone alongside an IPC_RMID.
 *
 * pte_count model (mirrors fork CoW, kernel/proc/fork.cpp): alloc_pages sets
 * each page to 1 (the segment's own reference); shmat adds +1 per page; each
 * unmap / address-space teardown subtracts 1.  So teardown never reaches 0
 * while the segment exists -- only the segment's explicit free does.
 *
 * Namespace: cinux::syscall
 */

#include "kernel/syscall/sys_shm.hpp"

#include <stdint.h>

#include "kernel/arch/x86_64/memory_layout.hpp"
#include "kernel/arch/x86_64/paging_config.hpp"
#include "kernel/arch/x86_64/user_access.hpp"  // copy_to_user (IPC_STAT)
#include "kernel/errno.hpp"
#include "kernel/ipc/shm.hpp"
#include "kernel/lib/aslr.hpp"
#include "kernel/mm/address_space.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/vma.hpp"
#include "kernel/proc/scheduler.hpp"

namespace cinux::syscall {

namespace {

using cinux::ipc::ShmRegistry;
using cinux::ipc::kShmMaxPages;

constexpr uint64_t kPageSize = 4096;

constexpr uint64_t align_up(uint64_t v) {
    return (v + kPageSize - 1) & ~(kPageSize - 1);
}

constexpr uint64_t pages_for(uint64_t size) {
    return align_up(size) / kPageSize;
}

/// Resolve the current task's tid, or 0 when there is no current task (the
/// ring-0 test installs a throwaway Task; production always has one).
uint32_t current_tid() {
    auto* t = cinux::proc::Scheduler::current();
    return t != nullptr ? static_cast<uint32_t>(t->tid) : 0;
}

/// Build the PTE flag set for an shmat mapping (W^X data page).
uint64_t shm_pte_flags(bool readonly) {
    using namespace cinux::arch;
    uint64_t f = FLAG_PRESENT | FLAG_USER | FLAG_NX;
    if (!readonly) {
        f |= FLAG_WRITABLE;
    }
    return f;
}

}  // namespace

// ============================================================
// shmget
// ============================================================

int64_t sys_shmget(uint64_t key, uint64_t size, uint64_t shmflg, uint64_t, uint64_t, uint64_t) {
    if (size == 0) {
        return -kEinval;
    }
    uint64_t page_count = pages_for(size);
    if (page_count > kShmMaxPages) {
        return -kEinval;
    }

    const int  ikey  = static_cast<int>(key);
    const bool creat = (shmflg & cinux::ipc::kIpcCreat) != 0;
    const bool excl  = (shmflg & cinux::ipc::kIpcExcl) != 0;

    // Named segment: consult the registry first.
    if (ikey != cinux::ipc::kIpcPrivate) {
        auto found = ShmRegistry::instance().find_by_key(ikey);
        if (found.ok()) {
            if (excl) {
                return -kEexist;  // IPC_EXCL + existing key
            }
            return found.value();  // reopen the existing shmid
        }
        if (!found.ok() && found.error() == cinux::lib::Error::NotFound && !creat) {
            return -kEnoent;  // no IPC_CREAT and the key is unknown
        }
    }

    // Allocate the backing frames.  alloc_pages rounds up to a buddy order;
    // each page starts refcount=1 (ownership), pte_count=0.
    uint64_t phys_base = cinux::mm::g_pmm.alloc_pages(page_count);
    if (phys_base == 0) {
        return -kEnomem;
    }

    auto reg = ShmRegistry::instance().add(ikey, size, phys_base, page_count,
                                           static_cast<uint16_t>(shmflg & 0x1FF));
    if (!reg.ok()) {
        cinux::mm::g_pmm.free_pages(phys_base, page_count);  // roll back the allocation
        return -to_errno(reg.error());
    }

    ShmRegistry::instance().segment(reg.value())->cpid = current_tid();
    return reg.value();
}

// ============================================================
// shmat
// ============================================================

int64_t sys_shmat(uint64_t shmid, uint64_t addr, uint64_t shmflg, uint64_t, uint64_t, uint64_t) {
    auto* task = cinux::proc::Scheduler::current();
    if (task == nullptr || task->addr_space == nullptr) {
        return -kEinval;
    }

    const uint32_t tid  = current_tid();
    auto           snap = ShmRegistry::instance().attach(static_cast<int>(shmid), tid);
    if (!snap.ok()) {
        return -kEinval;  // invalid id or being torn down
    }
    const cinux::ipc::ShmSegment seg = snap.value();
    const uint64_t               len = seg.page_count * kPageSize;
    const bool readonly              = (shmflg & cinux::ipc::kShmRdonly) != 0 ||
                                       (seg.mode & 0x80) == 0; /* SHM_RDONLY bit or no write perm */

    // Pick the virtual address: caller-supplied (page-aligned, in window) or a
    // fresh gap chosen via the VMA store (ASLR-jittered like mmap).
    uint64_t virt = 0;
    if (addr != 0) {
        uint64_t a = addr;
        if ((shmflg & cinux::ipc::kShmRnd) != 0) {
            a &= ~(kPageSize - 1);  // SHMLBA == PAGE on x86_64
        }
        if ((a & (kPageSize - 1)) != 0 || a < cinux::arch::USER_MMAP_BASE ||
            a + len > cinux::arch::USER_MMAP_END || a + len < a) {
            ShmRegistry::instance().detach(static_cast<int>(shmid), tid, nullptr);
            return -kEinval;
        }
        virt = a;
        static_cast<void>(task->addr_space->vmas().remove(virt, virt + len));  // drop any overlap
    } else {
        const uint64_t hint = cinux::arch::USER_MMAP_BASE + cinux::lib::aslr_mmap_offset();
        auto           area = task->addr_space->vmas().find_free_area(hint, len);
        if (!area.ok()) {
            ShmRegistry::instance().detach(static_cast<int>(shmid), tid, nullptr);
            return -to_errno(area.error());
        }
        virt = area.value();
        if (virt + len > cinux::arch::USER_MMAP_END || virt + len < virt) {
            ShmRegistry::instance().detach(static_cast<int>(shmid), tid, nullptr);
            return -kEnomem;
        }
    }

    // Record the VMA so shmdt can locate the mapping and find_free_area avoids
    // it.  SHM pages are eagerly mapped below; no demand-paging path runs here.
    using cinux::mm::VmaFlags;
    VmaFlags vf = VmaFlags::Read | VmaFlags::Shared;
    if (!readonly) {
        vf |= VmaFlags::Write;
    }
    auto ir = task->addr_space->vmas().insert(virt, virt + len, vf);
    if (!ir.ok()) {
        ShmRegistry::instance().detach(static_cast<int>(shmid), tid, nullptr);
        return -to_errno(ir.error());
    }

    // Eagerly map each segment frame into this address space and bump its
    // batch 3: install bumps the mapping counter (pte_count) AND takes a
    // map-ownership ref (refcount); the segment's own ref (alloc baseline=1)
    // keeps the page alive across shmdt so only the final IPC_RMID frees it.
    const uint64_t pte_flags = shm_pte_flags(readonly);
    for (uint64_t i = 0; i < seg.page_count; ++i) {
        const uint64_t v = virt + i * kPageSize;
        const uint64_t p = seg.phys_base + i * kPageSize;
        if (!task->addr_space->map(v, p, pte_flags)) {
            // Roll back the pages we already mapped + the VMA + the attach.
            for (uint64_t j = 0; j < i; ++j) {
                const uint64_t pv = seg.phys_base + j * kPageSize;
                task->addr_space->unmap(virt + j * kPageSize);
                static_cast<void>(cinux::mm::g_pmm.pte_count_dec_and_test(pv));  // undo install
            }
            static_cast<void>(task->addr_space->vmas().remove(virt, virt + len));
            ShmRegistry::instance().detach(static_cast<int>(shmid), tid, nullptr);
            return -kEnomem;
        }
        cinux::mm::g_pmm.pte_count_inc(p);
        cinux::mm::g_pmm.refcount_inc(p);
    }

    return static_cast<int64_t>(virt);
}

// ============================================================
// shmdt
// ============================================================

int64_t sys_shmdt(uint64_t addr, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    auto* task = cinux::proc::Scheduler::current();
    if (task == nullptr || task->addr_space == nullptr) {
        return -kEinval;
    }
    if ((addr & (kPageSize - 1)) != 0) {
        return -kEinval;  // shmdt takes the base address a prior shmat returned
    }

    // Resolve the segment via the caller's address (the exact base shmat
    // returned).  We deliberately do NOT use vma->start / vma->end for the
    // length: vmas().insert() coalesces adjacent same-flags mappings, so two
    // back-to-back SHM attachments in one address space share a single VMA and
    // its [start,end) would span more than this segment.  The segment's own
    // page_count is the authoritative teardown length; remove() then punches a
    // clean hole even out of a merged VMA.
    const uint64_t phys_base = task->addr_space->translate(addr);
    if (phys_base == 0) {
        return -kEinval;  // nothing mapped at this address
    }
    auto shmid_r = ShmRegistry::instance().find_by_phys(phys_base);
    if (!shmid_r.ok()) {
        return -kEinval;  // not a shm segment start (unmapped or mid-segment)
    }
    const int                     shmid = shmid_r.value();
    const cinux::ipc::ShmSegment* seg   = ShmRegistry::instance().segment(shmid);
    if (seg == nullptr || seg->page_count == 0) {
        return -kEinval;
    }

    const uint64_t pages = seg->page_count;
    const uint64_t len   = pages * kPageSize;
    for (uint64_t i = 0; i < pages; ++i) {
        const uint64_t v = addr + i * kPageSize;
        const uint64_t p = phys_base + i * kPageSize;
        task->addr_space->unmap(v);
        // Undo the attach-time inc; the segment owns the frames (the alloc-set
        // ref keeps pte_count > 0), so dec never frees here.
        static_cast<void>(cinux::mm::g_pmm.pte_count_dec_and_test(p));
    }

    static_cast<void>(task->addr_space->vmas().remove(addr, addr + len));

    // Drop the attach; if this was the last one on a marked segment, the
    // registry hands back the phys base for us to free.
    uint64_t free_count = 0;
    uint64_t free_phys  = ShmRegistry::instance().detach(shmid, current_tid(), &free_count);
    if (free_phys != 0) {
        cinux::mm::g_pmm.free_pages(free_phys, free_count);
    }
    return 0;
}

// ============================================================
// shmctl (kernel + boundary variants)
// ============================================================

int64_t do_shmctl_kernel(uint64_t shmid, uint64_t cmd, cinux::ipc::shmid_ds* out) {
    using namespace cinux::ipc;
    switch (static_cast<int>(cmd)) {
    case kIpcStat: {
        auto r = ShmRegistry::instance().stat(static_cast<int>(shmid), out);
        return r.ok() ? 0 : -to_errno(r.error());
    }
    case kIpcRmid: {
        uint64_t count = 0;
        uint64_t phys  = ShmRegistry::instance().mark_removal(static_cast<int>(shmid), &count);
        if (phys != 0) {
            cinux::mm::g_pmm.free_pages(phys, count);
        }
        return 0;  // marking a stale id is a benign no-op, like Linux
    }
    default:
        return -kEnosys;  // IPC_SET / IPC_INFO deferred
    }
}

int64_t sys_shmctl(uint64_t shmid, uint64_t cmd, uint64_t buf, uint64_t, uint64_t, uint64_t) {
    using namespace cinux::ipc;

    if (cmd == static_cast<uint64_t>(kIpcStat)) {
        shmid_ds kds;
        int64_t  rc = do_shmctl_kernel(shmid, cmd, &kds);
        if (rc < 0) {
            return rc;
        }
        if (buf != 0 &&
            !cinux::user::copy_to_user(reinterpret_cast<void*>(buf), &kds, sizeof(kds))) {
            return -kEfault;
        }
        return 0;
    }

    // IPC_RMID takes no output buffer; other commands are handled (or refused)
    // by the kernel variant.
    return do_shmctl_kernel(shmid, cmd, nullptr);
}

}  // namespace cinux::syscall
