#include "kernel/proc/sync.hpp"

#include <atomic>
#include <type_traits>

#include "cinux/assert.hpp"
#include "kernel/arch/x86_64/instructions.hpp"
#include "kernel/arch/x86_64/irq_guard.hpp"
#include "kernel/arch/x86_64/per_cpu.hpp"
#include "kernel/proc/lockdep.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/task.hpp"

namespace cinux::proc {

unsigned int SpinLedger::depth() const {
    return depth_[arch::per_cpu::PerCpu::self().cpu_id];
}

SpinLedger& DefaultSpinLedger() {
    static constinit SpinLedger instance{};
    return instance;
}

namespace {

template <typename Order, typename Dump>
void record_acquire(Order& order, const void* lock, Dump&& dump) {
    if constexpr (std::is_same_v<Order, LockOrder>) {
        const LockOrder::Status kOutcome = order.acquire(lock);
        if (kOutcome != LockOrder::Status::kOk) {
            dump();
        }
        cinux::base::safety::Check(kOutcome != LockOrder::Status::kCapacity,
                                   "lockdep acquisition exhausted identity or held-chain capacity");
        cinux::base::safety::Check(kOutcome == LockOrder::Status::kOk,
                                   "lockdep acquisition failed: cycle, recursion or capacity");
    }
}

template <typename Order, typename Dump>
void record_release(Order& order, const void* lock, Dump&& dump) {
    if constexpr (std::is_same_v<Order, LockOrder>) {
        const LockOrder::Status kOutcome = order.release(lock);
        if (kOutcome != LockOrder::Status::kOk) {
            dump();
        }
        cinux::base::safety::Check(kOutcome == LockOrder::Status::kOk,
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

void SpinLedger::name_lock(const void* lock, const char* label) {
    order_.name_lock(lock, label);
}

void SpinLedger::dump_held() {
    order_.dump_held();
}

void SpinLedger::note_acquire(const void* lock) {
    record_acquire(order_, lock, [this] { dump_held(); });
    ++depth_[arch::per_cpu::PerCpu::self().cpu_id];
}

void SpinLedger::note_release(const void* lock) {
    auto& depth = depth_[arch::per_cpu::PerCpu::self().cpu_id];
    cinux::base::safety::Check(depth != 0, "spin ledger underflow");
    record_release(order_, lock, [this] { dump_held(); });
    --depth;
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
    const auto kIrq = cinux::arch::SaveAndDisableIrq();
    const auto kCpu = static_cast<int>(arch::per_cpu::PerCpu::self().cpu_id);
    cinux::base::safety::Check(owner_cpu_.load(std::memory_order_relaxed) != kCpu,
                               "spinlock re-entered on one core");
    while (held_.test_and_set(std::memory_order_acquire)) {
        cinux::arch::CpuRelax();
    }
    irq_state_ = kIrq;
    owner_cpu_.store(kCpu, std::memory_order_relaxed);
    SpinLedger& ledger = ledger_ != nullptr ? *ledger_ : DefaultSpinLedger();
    ledger.note_acquire(this);
    holder_ = Scheduler::self().current();
}

void Spinlock::unlock() {
    const auto kIrq = irq_state_;
    cinux::base::safety::Check(holder_ == Scheduler::self().current(),
                               "spinlock released by a non-holder");
    holder_            = nullptr;
    SpinLedger& ledger = ledger_ != nullptr ? *ledger_ : DefaultSpinLedger();
    ledger.note_release(this);
    owner_cpu_.store(-1, std::memory_order_relaxed);
    held_.clear(std::memory_order_release);
    cinux::arch::RestoreIrq(kIrq);
}

const char* Spinlock::holder_name() const {
    return holder_ != nullptr ? holder_->name : "irq/main";
}

void Spinlock::set_name(const char* label) {
    name_              = label;
    SpinLedger& ledger = ledger_ != nullptr ? *ledger_ : DefaultSpinLedger();
    ledger.name_lock(this, label);
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
    // Release the spinlock before switching, but keep IRQs masked until
    // park_current() has committed the task's blocked state.
    const cinux::arch::IrqGuard kParkGuard;
    Task* const                 kSelf = Scheduler::self().current();
    {
        const SpinGuard kGuard(spin_);
        if (owner_ == nullptr) {
            owner_ = kSelf;
            return;
        }
        waiters_.push_back(*kSelf);
        Scheduler::self().prepare_to_wait();
    }
    Scheduler::self().schedule_blocked();
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

void Semaphore::retire() {
    cinux::base::safety::Check(waiters_.empty(), "retired semaphore still has waiters");
    spin_.retire();
}

void Semaphore::wait() {
    const cinux::arch::IrqGuard kParkGuard;
    Task* const                 kSelf = Scheduler::self().current();
    {
        const SpinGuard kGuard(spin_);
        if (count_ > 0) {
            count_--;
            return;
        }
        waiters_.push_back(*kSelf);
        Scheduler::self().prepare_to_wait();
    }
    Scheduler::self().schedule_blocked();
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

bool Completion::finish(bool success) {
    const SpinGuard kGuard(spin_);
    if (state_ != State::kPending) {
        return false;
    }
    state_            = success ? State::kSuccess : State::kFailed;
    Task* const kNext = waiters_.pop_head();
    if (kNext != nullptr) {
        Scheduler::self().unblock(*kNext);
    }
    return true;
}

bool Completion::wait() {
    const cinux::arch::IrqGuard kParkGuard;
    Task* const                 kSelf = Scheduler::self().current();
    {
        const SpinGuard kGuard(spin_);
        if (state_ != State::kPending) {
            return state_ == State::kSuccess;
        }
        waiters_.push_back(*kSelf);
        Scheduler::self().prepare_to_wait();
    }
    Scheduler::self().schedule_blocked();
    {
        const SpinGuard kGuard(spin_);
        return state_ == State::kSuccess;
    }
}

bool Completion::finished() const {
    const SpinGuard kGuard(spin_);
    return state_ != State::kPending;
}

bool Completion::success() const {
    const SpinGuard kGuard(spin_);
    return state_ == State::kSuccess;
}

void Completion::rearm() {
    const SpinGuard kGuard(spin_);
    cinux::base::safety::Check(waiters_.empty(), "rearm with a waiter still parked");
    state_ = State::kPending;
}

}  // namespace cinux::proc
