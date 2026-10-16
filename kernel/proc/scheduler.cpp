#include "kernel/proc/scheduler.hpp"

#include "cinux/assert.hpp"
#include "cinux/container/self_list.hpp"
#include "kernel/arch/x86_64/irq_guard.hpp"
#include "kernel/proc/sched_class.hpp"
#include "kernel/proc/signals.hpp"
#include "kernel/proc/sync.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/syscall/signal.hpp"

namespace cinux::proc {

bool Scheduler::sink_ready() const {
    return sink_ != nullptr;
}

void Scheduler::install_class(SchedulingClass& cls) {
    const cinux::arch::IrqGuard kGuard;
    for (const SchedulingClass* resident : classes_.view()) {
        if (resident == &cls) {
            return;
        }
    }
    cinux::base::safety::Check(classes_.push(&cls), "scheduling class registry full");
}

unsigned int Scheduler::class_count() const {
    return classes_.size();
}

SchedulingClass* Scheduler::class_at(unsigned int index) const {
    if (index >= classes_.size()) {
        return nullptr;
    }
    return classes_.view()[index];
}

void Scheduler::seat(Task& task) {
    const cinux::arch::IrqGuard kGuard;
    cinux::base::safety::Check(sink_ready(), "scheduler has no sink installed");
    cinux::base::safety::Check(!classes_.empty(), "no scheduling class installed");
    if (task.sched_class == nullptr) {
        task.sched_class = classes_.view()[0];
    }
    bool registered = false;
    for (const SchedulingClass* resident : classes_.view()) {
        if (resident == task.sched_class) {
            registered = true;
            break;
        }
    }
    cinux::base::safety::Check(registered, "task points at an unregistered class");
    task.pid   = next_pid_++;
    task.state = TaskState::kReady;
    if (task.parent != nullptr) {
        offspring_.push_back(task);
    }
    cinux::base::safety::Check(task.sched_class->enqueue(task), "ready queue full at seat");
}

Task* Scheduler::pick_next() {
    for (SchedulingClass* cls : classes_.view()) {
        Task* next = cls->pick();
        if (next != nullptr) {
            return next;
        }
    }
    return nullptr;
}

Task* Scheduler::current() const {
    return current_;
}

unsigned long long Scheduler::offspring_count() const {
    return offspring_.size();
}

Task* Scheduler::find_by_pid(unsigned long long pid) {
    const cinux::arch::IrqGuard kGuard;
    for (Task* scan = offspring_.head(); scan != nullptr; scan = offspring_.after(*scan)) {
        if (scan->pid == pid) {
            return scan;
        }
    }
    return nullptr;
}

void Scheduler::yield() {
    const cinux::arch::IrqGuard kGuard;
    cinux::base::safety::Check(current_ != nullptr, "yield called outside any task");
    Task* const kPrev = current_;
    kPrev->state      = TaskState::kReady;
    cinux::base::safety::Check(kPrev->sched_class->enqueue(*kPrev), "ready queue full at yield");
    rotate_from_interrupt();
}

void Scheduler::rotate_from_interrupt() {
    Task* const kPrev = current_;
    Task*       next  = pick_next();
    cinux::base::safety::Check(next != nullptr, "ready classes lost the outgoing task");
    if (next == kPrev) {
        kPrev->state = TaskState::kRunning;
        return;
    }
    current_     = next;
    next->state  = TaskState::kRunning;
    slice_ticks_ = 0;
    switch_tasks_(sink_, *kPrev, *next);
    reap_pending();
}

void Scheduler::on_timer_tick() {
    if (current_ == nullptr || !preemption_) {
        return;
    }
    slice_ticks_++;
    if (current_->sched_class->wants_rotate(slice_ticks_)) {
        slice_ticks_ = 0;
        rotate_due_  = true;
    }
}

void Scheduler::maybe_preempt() {
    if (!rotate_due_ || current_ == nullptr || !preemption_ || preempt_disable_depth_ != 0) {
        return;
    }
    const cinux::arch::IrqGuard kGuard;
    if (!rotate_due_ || current_ == nullptr) {
        return;
    }
    rotate_due_     = false;
    current_->state = TaskState::kReady;
    cinux::base::safety::Check(current_->sched_class->enqueue(*current_),
                               "ready queue full at preemption");
    rotate_from_interrupt();
}

void Scheduler::set_preemption(bool enabled) {
    preemption_  = enabled;
    slice_ticks_ = 0;
    rotate_due_  = false;
}

void Scheduler::preempt_disable() {
    preempt_disable_depth_++;
}

void Scheduler::preempt_enable() {
    preempt_disable_depth_--;
}

void Scheduler::exit_current(long long code) {
    const cinux::arch::IrqGuard kGuard;
    Task* const                 kPrev = current_;
    cinux::base::safety::Check(kPrev != nullptr, "exit called outside any task");
    kPrev->exit_code = code;
    detach_offspring(*kPrev);
    if (kPrev->parent != nullptr) {
        kPrev->state = TaskState::kZombie;
        SendSignal(*kPrev->parent, cinux::syscall::kSigChld);
        if (waiters_.remove(*kPrev->parent)) {
            unblock(*kPrev->parent);
        }
    } else {
        kPrev->state = TaskState::kDead;
        reap_pending();
        pending_reap_ = kPrev;
    }
    Task* next = pick_next();
    if (next != nullptr) {
        current_     = next;
        next->state  = TaskState::kRunning;
        slice_ticks_ = 0;
        switch_tasks_(sink_, *kPrev, *next);
    } else {
        current_ = nullptr;
        return_to_main_(sink_, *kPrev);
    }
}

Task* Scheduler::take_zombie_child(Task& watcher, long long wanted, bool& has_eligible) {
    Task*                match = nullptr;
    decltype(offspring_) keep{};
    Task*                scan            = nullptr;
    has_eligible                         = false;
    const bool               kAnyChild   = wanted == -1;
    const bool               kOwnGroup   = wanted == 0;
    const bool               kNamedGroup = wanted < -1;
    const unsigned long long kWantedPid =
        kAnyChild || kOwnGroup || kNamedGroup ? 0ULL : static_cast<unsigned long long>(wanted);
    const unsigned long long kGroup =
        kNamedGroup ? static_cast<unsigned long long>(-wanted) : watcher.pgid;
    while ((scan = offspring_.pop_head()) != nullptr) {
        if (scan->parent != &watcher) {
            keep.push_back(*scan);
            continue;
        }
        const bool kNamed    = kAnyChild || scan->pid == kWantedPid;
        const bool kGrouped  = (kOwnGroup || kNamedGroup) && scan->pgid == kGroup;
        const bool kEligible = kNamed || kGrouped;
        has_eligible         = has_eligible || kEligible;
        const bool kHit      = kEligible && scan->state == TaskState::kZombie;
        if (kHit && match == nullptr) {
            match = scan;
        } else {
            keep.push_back(*scan);
        }
    }
    while ((scan = keep.pop_head()) != nullptr) {
        offspring_.push_back(*scan);
    }
    return match;
}

void Scheduler::detach_offspring(Task& dying) {
    decltype(offspring_) keep{};
    Task*                scan = nullptr;
    while ((scan = offspring_.pop_head()) != nullptr) {
        if (scan->parent != &dying) {
            keep.push_back(*scan);
            continue;
        }
        scan->parent = nullptr;
        if (scan->state == TaskState::kZombie) {
            reclaim_(sink_, *scan);
        }
        // Nobody can ever wait for a parentless task again, so it
        // leaves the roll here instead of lingering until its own exit.
        // A zombie grandchild already left via reclaim above; a live
        // one simply drops off the list by not re-entering keep.
    }
    while ((scan = keep.pop_head()) != nullptr) {
        offspring_.push_back(*scan);
    }
}

bool Scheduler::wait_child(long long wanted, bool no_hang, ChildReceipt& receipt) {
    for (;;) {
        const cinux::arch::IrqGuard kGuard;
        Task* const                 kSelf = current_;
        cinux::base::safety::Check(kSelf != nullptr, "wait called outside any task");
        bool  has_any_child = false;
        Task* match         = take_zombie_child(*kSelf, wanted, has_any_child);
        if (match != nullptr) {
            receipt.pid  = match->pid;
            receipt.code = match->exit_code;
            reclaim_(sink_, *match);
            return true;
        }
        if (!has_any_child) {
            return false;
        }
        if (no_hang) {
            receipt.pid = 0;
            return true;
        }
        cinux::base::safety::Check(DefaultSpinLedger().depth() == 0,
                                   "task parked while holding a spinlock");
        waiters_.push_back(*kSelf);
        park_current();
    }
}

void Scheduler::block_current() {
    const cinux::arch::IrqGuard kGuard;
    cinux::base::safety::Check(current_ != nullptr, "block called outside any task");
    cinux::base::safety::Check(DefaultSpinLedger().depth() == 0,
                               "task parked while holding a spinlock");
    park_current();
}

void Scheduler::park_current() {
    Task* const kPrev = current_;
    kPrev->state      = TaskState::kBlocked;
    Task* next        = pick_next();
    if (next != nullptr) {
        current_     = next;
        next->state  = TaskState::kRunning;
        slice_ticks_ = 0;
        switch_tasks_(sink_, *kPrev, *next);
        reap_pending();
        return;
    }
    current_ = nullptr;
    return_to_main_(sink_, *kPrev);
}

void Scheduler::unblock(Task& task) {
    const cinux::arch::IrqGuard kGuard;
    cinux::base::safety::Check(task.state == TaskState::kBlocked,
                               "unblock of a task that is not parked");
    task.state = TaskState::kReady;
    cinux::base::safety::Check(task.sched_class->enqueue(task), "ready queue full at unblock");
}

void Scheduler::stop_current() {
    const cinux::arch::IrqGuard kGuard;
    Task* const                 kSelf = current_;
    cinux::base::safety::Check(kSelf != nullptr, "stop called outside any task");
    cinux::base::safety::Check(DefaultSpinLedger().depth() == 0,
                               "task stopped while holding a spinlock");
    kSelf->state = TaskState::kStopped;
    Task* next   = pick_next();
    if (next != nullptr) {
        current_     = next;
        next->state  = TaskState::kRunning;
        slice_ticks_ = 0;
        switch_tasks_(sink_, *kSelf, *next);
        reap_pending();
        return;
    }
    current_ = nullptr;
    return_to_main_(sink_, *kSelf);
}

void Scheduler::resume(Task& task) {
    const cinux::arch::IrqGuard kGuard;
    cinux::base::safety::Check(task.state == TaskState::kStopped,
                               "resume of a task that is not stopped");
    task.state = TaskState::kReady;
    cinux::base::safety::Check(task.sched_class->enqueue(task), "ready queue full at resume");
}

bool Scheduler::withdraw(Task& task) {
    const cinux::arch::IrqGuard kGuard;
    if (task.state != TaskState::kReady) {
        return false;
    }
    const bool kQueued = task.sched_class->remove(task);
    cinux::base::safety::Check(kQueued, "ready queue lost a task the stop signal named");
    task.state = TaskState::kStopped;
    return true;
}

void Scheduler::run_until_done() {
    const cinux::arch::IrqGuard kGuard;
    cinux::base::safety::Check(current_ == nullptr, "run_until_done called from a task");
    cinux::base::safety::Check(sink_ready(), "scheduler has no sink installed");
    Task* next = nullptr;
    while ((next = pick_next()) != nullptr) {
        current_     = next;
        next->state  = TaskState::kRunning;
        slice_ticks_ = 0;
        enter_from_main_(sink_, *next);
        reap_pending();
    }
}

void Scheduler::reap_pending() {
    Task* const kDead = pending_reap_;
    if (kDead == nullptr) {
        return;
    }
    pending_reap_ = nullptr;
    reclaim_(sink_, *kDead);
}

}  // namespace cinux::proc
