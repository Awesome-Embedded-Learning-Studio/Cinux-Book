#include "kernel/proc/futex.hpp"

#include <array>
#include <utility>

#include "cinux/container/self_list.hpp"
#include "cinux/ptr.hpp"
#include "cinux/result.hpp"
#include "cinux/singleton.hpp"
#include "kernel/arch/x86_64/context.hpp"
#include "kernel/arch/x86_64/irq_guard.hpp"
#include "kernel/mm/vma.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/sync.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/syscall/syscall.hpp"

namespace cinux::proc::futex {

namespace {

/// Bucket count; the index is (address >> 2) scaled down to this many.
constexpr unsigned long long kBucketCount = 256;

/// The one table: a single gate over every bucket, because one core
/// never holds two buckets apart and the lockdep story stays one lock.
class FutexTable : public cinux::base::Singleton<FutexTable> {
public:
    FutexTable() = default;

    /// The gate every bucket update walks through.
    Spinlock gate;
    /// One wait list per bucket; a task parks holding its exact
    /// address so a bucket mate on a different word is never woken.
    std::array<cinux::base::container::SelfList<Task, &Task::futex_wait_link>, kBucketCount>
        buckets{};

    /// The bucket a word lands in: word granularity, spread by shift.
    [[nodiscard]] static unsigned long long bucket_for(unsigned long long uaddr) {
        return (uaddr >> 2ULL) & (kBucketCount - 1ULL);
    }
};

/// The constinit singleton cannot name its lock in a constructor, so
/// the first caller through here does it; repeating the label is
/// harmless and every later call is a plain reference fetch.
FutexTable& shared_table() {
    static bool named = false;
    FutexTable& table = FutexTable::self();
    if (!named) {
        table.gate.set_name("futex table");
        named = true;
    }
    return table;
}

}  // namespace

base::KernelError WaitOnUserWord(unsigned long long uaddr, unsigned int expected) {
    const cinux::arch::IrqGuard kQuiet;
    Task* const                 kSelf = Scheduler::self().current();
    FutexTable&                 table = shared_table();
    {
        const SpinGuard   kGuard(table.gate);
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto const* const kWord = reinterpret_cast<volatile const unsigned int*>(uaddr);
        if (*kWord != expected) {
            return base::KernelError::kWouldBlock;
        }
        kSelf->futex_uaddr = uaddr;
        table.buckets[FutexTable::bucket_for(uaddr)].push_back(*kSelf);
    }
    Scheduler::self().block_current();
    return base::KernelError::kOk;
}

long long WakeUserWord(unsigned long long uaddr, unsigned long long count) {
    FutexTable&                 table = shared_table();
    long long                   woken = 0;
    const cinux::arch::IrqGuard kQuiet;
    const SpinGuard             kGuard(table.gate);
    auto&                       bucket = table.buckets[FutexTable::bucket_for(uaddr)];
    Task*                       node   = bucket.head();
    while (node != nullptr && std::cmp_less(woken, count)) {
        Task* const kNext = bucket.after(*node);
        if (node->futex_uaddr == uaddr) {
            bucket.remove(*node);
            Scheduler::self().unblock(*node);
            woken++;
        }
        node = kNext;
    }
    return woken;
}

void ClearTidWord(Task& dying) {
    const unsigned long long kWord          = dying.clear_child_tid;
    dying.clear_child_tid                   = 0;
    constexpr unsigned long long kWordBytes = sizeof(unsigned int);
    if (kWord < kWordBytes || kWord + kWordBytes > syscall::kUserAddressLimit ||
        !arch::IsCanonicalAddress(kWord) || !dying.vmas.holds()) {
        return;
    }
    const mm::VmaRegion* const kRegion = dying.vmas->find(static_cast<unsigned long>(kWord));
    if (kRegion == nullptr || !kRegion->writable ||
        kRegion->end < static_cast<unsigned long>(kWord + kWordBytes)) {
        return;
    }
    *cinux::base::PtrAt<volatile unsigned int>(kWord) = 0;
    static_cast<void>(WakeUserWord(kWord, 1));
}

}  // namespace cinux::proc::futex
