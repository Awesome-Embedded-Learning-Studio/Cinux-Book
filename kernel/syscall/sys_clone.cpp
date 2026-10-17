#include "kernel/syscall/sys_clone.hpp"

#include "cinux/ptr.hpp"
#include "kernel/arch/x86_64/context.hpp"
#include "kernel/proc/clone.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::syscall {

namespace {

/// Every flag bit this handler understands; anything outside the mask
/// is refused whole rather than half-served.
constexpr unsigned long long kServedFlags = kCloneVm | kCloneFiles | kCloneThread | kCloneSetTls |
                                            kCloneParentSetTid | kCloneChildClearTid |
                                            kCloneChildSetTid;

}  // namespace

long long HandleClone(const SyscallFrame& frame) {
    const unsigned long long kFlags = frame.rdi;
    if ((kFlags & ~kServedFlags) != 0) {
        return -kEinval;
    }
    const unsigned long long kStack = frame.rsi;
    if (kStack == 0 || kStack >= kUserAddressLimit) {
        return -kEinval;
    }
    if ((kFlags & kCloneSetTls) != 0 && !arch::IsCanonicalAddress(frame.r8)) {
        return -kEinval;
    }
    const unsigned long long kParentTid = frame.rdx;
    const unsigned long long kChildTid  = frame.r10;
    if ((kFlags & kCloneParentSetTid) != 0 &&
        !UserRangeWritable(kParentTid, sizeof(unsigned int))) {
        return -kEfault;
    }
    if ((kFlags & (kCloneChildSetTid | kCloneChildClearTid)) != 0 &&
        !UserRangeWritable(kChildTid, sizeof(unsigned int))) {
        return -kEfault;
    }
    if ((kFlags & kCloneChildSetTid) != 0 && (kFlags & kCloneVm) == 0) {
        return -kEinval;
    }
    const proc::ClonePlan kPlan{
        .flags      = kFlags,
        .user_stack = kStack,
        .tls        = frame.r8,
    };
    proc::Task* const kChild =
        proc::BuildCloneChild(reinterpret_cast<unsigned long long>(&frame), kPlan);
    if (kChild == nullptr) {
        return -kEnomem;
    }
    if ((kFlags & kCloneChildClearTid) != 0) {
        kChild->clear_child_tid = kChildTid;
    }
    proc::Scheduler::self().seat(*kChild);
    if ((kFlags & kCloneThread) != 0) {
        kChild->tgid = CurrentTask()->tgid;
    }
    const auto kTid = static_cast<unsigned int>(kChild->pid);
    if ((kFlags & kCloneParentSetTid) != 0) {
        *cinux::base::PtrAt<volatile unsigned int>(kParentTid) = kTid;
    }
    if ((kFlags & kCloneChildSetTid) != 0) {
        *cinux::base::PtrAt<volatile unsigned int>(kChildTid) = kTid;
    }
    return static_cast<long long>(kChild->pid);
}

}  // namespace cinux::syscall
