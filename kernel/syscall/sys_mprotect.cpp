#include "kernel/syscall/sys_mprotect.hpp"

#include "cinux/math.hpp"
#include "cinux/result.hpp"
#include "kernel/arch/x86_64/page.hpp"
#include "kernel/arch/x86_64/page_entry.hpp"
#include "kernel/arch/x86_64/registers.hpp"
#include "kernel/mm/address_space.hpp"
#include "kernel/mm/page_walk.hpp"
#include "kernel/mm/vma.hpp"
#include "kernel/mm/vmm.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::syscall {

long long HandleMprotect(const SyscallFrame& frame) {
    const proc::Task* const kSelf = CurrentTask();
    if (kSelf == nullptr || !kSelf->vmas.holds() || !kSelf->space.holds()) {
        return -kEnomem;
    }
    if (frame.rdi % cinux::arch::page::kSize != 0 || frame.rdi >= kUserAddressLimit ||
        frame.rsi > kUserAddressLimit - frame.rdi ||
        (frame.rdx & ~(kProtRead | kProtWrite | kProtExec)) != 0) {
        return -kEinval;
    }
    if (frame.rsi == 0) {
        return 0;
    }
    const unsigned long kStart = frame.rdi;
    const unsigned long kEnd =
        cinux::base::math::AlignUp(frame.rdi + frame.rsi, cinux::arch::page::kSize);
    if (kStart >= kEnd) {
        return -kEinval;
    }
    const mm::VmaProtection kTerms{
        .readable   = (frame.rdx & kProtRead) != 0,
        .writable   = (frame.rdx & kProtWrite) != 0,
        .executable = (frame.rdx & kProtExec) != 0,
    };
    const base::Result<void> kRewritten = kSelf->vmas->protect(kStart, kEnd, kTerms);
    if (!kRewritten.ok()) {
        return -ErrnoFrom(kRewritten.error());
    }
    mm::KernelTables tables;
    for (unsigned long spot = kStart; spot < kEnd; spot += cinux::arch::page::kSize) {
        const mm::walk::SlotAt kSlot = mm::walk::FindSlot(tables, kSelf->space->root(), spot);
        if (kSlot.entry == nullptr || kSlot.level != mm::walk::WalkLevel::kPageTable) {
            continue;
        }
        const cinux::arch::page::Entry kLeaf = *kSlot.entry;
        if (!kLeaf.has(cinux::arch::page::kPresent) || kLeaf.has(cinux::arch::page::kLarge)) {
            continue;
        }
        const bool               kShared = kLeaf.has(cinux::arch::page::kCow);
        cinux::arch::page::Entry bits =
            cinux::arch::page::UserLeafTerms(kTerms.writable && !kShared, kTerms.executable);
        if (!kTerms.readable && !kTerms.writable && !kTerms.executable) {
            bits = bits & ~cinux::arch::page::kUser;
        }
        if (kShared) {
            bits = bits | cinux::arch::page::kCow;
        }
        *kSlot.entry = cinux::arch::page::MakeTableEntry(mm::walk::EntryTarget(kLeaf), bits);
        cinux::arch::InvalidatePage(spot);
    }
    return 0;
}

}  // namespace cinux::syscall
