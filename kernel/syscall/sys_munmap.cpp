#include "kernel/syscall/sys_munmap.hpp"

#include "cinux/math.hpp"
#include "cinux/result.hpp"
#include "kernel/arch/x86_64/page.hpp"
#include "kernel/arch/x86_64/page_entry.hpp"
#include "kernel/arch/x86_64/registers.hpp"
#include "kernel/mm/address_space.hpp"
#include "kernel/mm/cow.hpp"
#include "kernel/mm/page_walk.hpp"
#include "kernel/mm/vma.hpp"
#include "kernel/mm/vmm.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::syscall {

long long HandleMunmap(const SyscallFrame& frame) {
    const proc::Task* const kSelf = CurrentTask();
    if (kSelf == nullptr || !kSelf->vmas.holds() || !kSelf->space.holds()) {
        return -kEnomem;
    }
    const unsigned long kStart = cinux::base::math::AlignDown(frame.rdi, cinux::arch::page::kSize);
    const unsigned long kEnd =
        cinux::base::math::AlignUp(frame.rdi + frame.rsi, cinux::arch::page::kSize);
    if (kStart >= kEnd) {
        return -kEinval;
    }
    const base::Result<void> kGone = kSelf->vmas->unregister_range(kStart, kEnd);
    if (!kGone.ok()) {
        return -ErrnoFrom(kGone.error());
    }
    mm::KernelTables tables;
    for (unsigned long spot = kStart; spot < kEnd; spot += cinux::arch::page::kSize) {
        const mm::walk::Entry kOld = mm::walk::UnmapPage(tables, kSelf->space->root(), spot);
        if (kOld.has(cinux::arch::page::kPresent)) {
            mm::cow::ReleaseFrame(mm::walk::EntryTarget(kOld));
            cinux::arch::InvalidatePage(spot);
        }
    }
    return 0;
}

}  // namespace cinux::syscall
