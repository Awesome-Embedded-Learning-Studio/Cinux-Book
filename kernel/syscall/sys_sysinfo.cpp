#include "kernel/syscall/sys_sysinfo.hpp"

#include "cinux/memory.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/uaccess.hpp"

namespace cinux::syscall {

namespace {

/// The hosted sysinfo record, x86_64 shape: two longs, three loads,
/// six memory words, two shorts, two high words, one unit word, and
/// the tail the struct rounds out with.
constexpr unsigned long kSysinfoBytes = 112;

/// Where each fact sits, locked against the hosted header by test.
constexpr unsigned long kTotalRamAt = 32;
constexpr unsigned long kFreeRamAt  = 40;
constexpr unsigned long kProcsAt    = 72;
constexpr unsigned long kMemUnitAt  = 100;

}  // namespace

long long HandleSysinfo(const SyscallFrame& frame) {
    if (frame.rdi == 0) {
        return -kEfault;
    }
    unsigned char out[kSysinfoBytes];
    base::SetBytes(out, 0, kSysinfoBytes);

    const unsigned long long kPageSize = 4096;
    const unsigned long      kTotal    = mm::Pmm::self().managed_page_count() * kPageSize;
    const unsigned long      kFree     = mm::Pmm::self().free_page_count() * kPageSize;
    base::CopyBytes(out + kTotalRamAt, &kTotal, sizeof(kTotal));
    base::CopyBytes(out + kFreeRamAt, &kFree, sizeof(kFree));

    unsigned long long leader = 0;
    unsigned long      count  = 0;
    while (proc::Scheduler::self().nth_group_leader(count, leader)) {
        ++count;
    }
    const auto kProcs = static_cast<unsigned short>(count);
    base::CopyBytes(out + kProcsAt, &kProcs, sizeof(kProcs));

    const unsigned int kUnit = 1;
    base::CopyBytes(out + kMemUnitAt, &kUnit, sizeof(kUnit));

    if (!uaccess::CopyToUser(frame.rdi, out, kSysinfoBytes)) {
        return -kEfault;
    }
    return 0;
}

}  // namespace cinux::syscall
