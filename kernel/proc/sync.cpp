#include "kernel/proc/sync.hpp"

#include <atomic>
#include <type_traits>

#include "cinux/assert.hpp"
#include "kernel/arch/x86_64/instructions.hpp"
#include "kernel/arch/x86_64/irq_guard.hpp"
#include "kernel/proc/lockdep.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/task.hpp"

namespace cinux::proc {

SpinLedger& DefaultSpinLedger() {
    static constinit SpinLedger instance{};
    return instance;
}

namespace {

template <typename Order>
void record_acquire(Order& order, const void* lock) {
    if constexpr (std::is_same_v<Order, LockOrder>) {
        cinux::base::safety::Check(order.acquire(lock) == LockOrder::Status::kOk,
                                   "lockdep acquisition failed: cycle, recursion or capacity");
    }
}

template <typename Order>
void record_release(Order& order, const void* lock) {
    if constexpr (std::is_same_v<Order, LockOrder>) {
        cinux::base::safety::Check(order.release(lock) == LockOrder::Status::kOk,
                                   "lockdep release failed: not held or out of order");
    }
}

template <typename Order>
void forget_lock(Order& order, const void* lock) {
    if constexpr (std::is_same_v<Order, LockOrder>) {
        cinux::base::safety::Check(order.forget(lock), "lockdep destroyed held lock");
    }
}

}  // namespace

void SpinLedger::note_acquire(const void* lock) {
    record_acquire(order_, lock);
    ++depth_;
}

void SpinLedger::note_release(const void* lock) {
    cinux::base::safety::Check(depth_ != 0, "spin ledger underflow");
    record_release(order_, lock);
    --depth_;
}

void SpinLedger::forget(const void* lock) {
    forget_lock(order_, lock);
}

void Spinlock::retire() {
    const cinux::arch::IrqGuard kGuard;
    cinux::base::safety::Check(!held_.test(std::memory_order_relaxed), "destroyed held spinlock");
    SpinLedger& ledger = ledger_ != nullptr ? *ledger_ : DefaultSpinLedger();
    ledger.forget(this);
}

void Spinlock::lock() {
    cinux::base::safety::Check(!held_.test(std::memory_order_relaxed),
                               "spinlock re-entered on one core");
    irq_state_ = cinux::arch::SaveAndDisableIrq();
    while (held_.test_and_set(std::memory_order_acquire)) {
        cinux::arch::CpuRelax();
    }
    SpinLedger& ledger = ledger_ != nullptr ? *ledger_ : DefaultSpinLedger();
    ledger.note_acquire(this);
    holder_ = Scheduler::self().current();
}

void Spinlock::unlock() {
    cinux::base::safety::Check(holder_ == Scheduler::self().current(),
                               "spinlock released by a non-holder");
    holder_            = nullptr;
    SpinLedger& ledger = ledger_ != nullptr ? *ledger_ : DefaultSpinLedger();
    ledger.note_release(this);
    held_.clear(std::memory_order_release);
    cinux::arch::RestoreIrq(irq_state_);
}

const char* Spinlock::holder_name() const {
    return holder_ != nullptr ? holder_->name : "irq/main";
}

SpinGuard::SpinGuard(Spinlock& lock) : lock_(lock) {
    lock_.lock();
}

SpinGuard::~SpinGuard() {
    lock_.unlock();
}

SpinGuard Spinlock::guard() {
    return SpinGuard(*this);
}

void Mutex::lock() {
    Task* const kSelf = Scheduler::self().current();
    {
        const SpinGuard kGuard(spin_);
        if (owner_ == nullptr) {
            owner_ = kSelf;
            return;
        }
        waiters_.push_back(*kSelf);
    }
    Scheduler::self().block_current();
}

void Mutex::unlock() {
    const SpinGuard kGuard(spin_);
    cinux::base::safety::Check(owner_ == Scheduler::self().current(),
                               "mutex released by a non-owner");
    Task* const kNext = waiters_.pop_head();
    if (kNext != nullptr) {
        owner_ = kNext;
        Scheduler::self().unblock(*kNext);
        return;
    }
    owner_ = nullptr;
}

MutexGuard::MutexGuard(Mutex& mutex) : mutex_(mutex) {
    mutex_.lock();
}

MutexGuard::~MutexGuard() {
    mutex_.unlock();
}

MutexGuard Mutex::guard() {
    return MutexGuard(*this);
}

Semaphore::Semaphore(long long initial) : count_(initial) {}

void Semaphore::wait() {
    Task* const kSelf = Scheduler::self().current();
    {
        const SpinGuard kGuard(spin_);
        if (count_ > 0) {
            count_--;
            return;
        }
        waiters_.push_back(*kSelf);
    }
    Scheduler::self().block_current();
}

void Semaphore::post() {
    const SpinGuard kGuard(spin_);
    Task* const     kNext = waiters_.pop_head();
    if (kNext != nullptr) {
        Scheduler::self().unblock(*kNext);
        return;
    }
    count_++;
}

}  // namespace cinux::proc
