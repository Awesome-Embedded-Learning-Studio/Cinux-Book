#include "kernel/syscall/sys_brk.hpp"

#include "cinux/assert.hpp"
#include "cinux/math.hpp"
#include "cinux/result.hpp"
#include "kernel/arch/x86_64/page.hpp"
#include "kernel/mm/vma.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::syscall {

long long HandleBrk(const SyscallFrame& frame) {
    const proc::Task* const kSelf = CurrentTask();
    if (kSelf == nullptr || !kSelf->vmas.holds()) {
        return 0;
    }
    const auto                        kBefore = kSelf->vmas->heap_end();
    const base::Result<unsigned long> kMoved  = kSelf->vmas->move_heap_end(frame.rdi);
    if (kMoved.ok()) {
        const auto kOldPage = base::math::AlignUp(kBefore, arch::page::kSize);
        const auto kNewPage = base::math::AlignUp(kMoved.value(), arch::page::kSize);
        if (kNewPage < kOldPage) {
            const auto kReleased = ReclaimUserSpan(*kSelf, kNewPage, kOldPage - kNewPage);
            base::safety::Check(kReleased.ok(), "brk shrink lost its retired pages");
        }
        return static_cast<long long>(kMoved.value());
    }
    return static_cast<long long>(kSelf->vmas->heap_end());
}

}  // namespace cinux::syscall
