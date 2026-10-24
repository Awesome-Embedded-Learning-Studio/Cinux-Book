#include "kernel/proc/scheduler.hpp"

#include <atomic>
#include <utility>

#include "cinux/assert.hpp"
#include "cinux/memory.hpp"
#include "kernel/arch/x86_64/irq_guard.hpp"
#include "kernel/arch/x86_64/per_cpu.hpp"
#include "kernel/arch/x86_64/smp.hpp"
#include "kernel/proc/futex.hpp"
#include "kernel/proc/sched_class.hpp"
#include "kernel/proc/signals.hpp"
#include "kernel/proc/sync.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/proc/task_snapshot.hpp"
#include "kernel/syscall/signal.hpp"

namespace cinux::proc {

namespace {
char state_letter(TaskState state) {
    switch (state) {
    case TaskState::kRunning:
    case TaskState::kReady:
        return 'R';
    case TaskState::kBlocked:
        return 'S';
    case TaskState::kStopped:
        return 'T';
    default:
        return 'Z';
    }
}
}  // namespace

arch::per_cpu::PerCpu& Scheduler::local() {
    return arch::per_cpu::PerCpu::self();
}

void Scheduler::QueuePolicy::enter(State& state) {
    state.irq = arch::SaveAndDisableIrq();
    if (local().queue_depth++ == 0) {
        state.scheduler.queue_lock_.lock();
    }
}

void Scheduler::QueuePolicy::exit(State& state) {
    if (--local().queue_depth == 0) {
        const bool kNotify     = local().notify_pending;
        local().notify_pending = false;
        state.scheduler.queue_lock_.unlock();
        if (kNotify) {
            arch::smp::NotifyRunnable();
        }
    }
    arch::RestoreIrq(state.irq);
}

void Scheduler::NoReschedulePolicy::enter([[maybe_unused]] State& state) {
    ++local().no_reschedule_depth;
}
void Scheduler::NoReschedulePolicy::exit([[maybe_unused]] State& state) {
    --local().no_reschedule_depth;
}

bool Scheduler::sink_ready() const {
    return sink_ != nullptr;
}

void Scheduler::install_class(SchedulingClass& cls) {
    const QueueGuard kGuard(*this);
    for (const SchedulingClass* resident : classes_.view()) {
        if (resident == &cls) {
            return;
        }
    }
    base::safety::Check(classes_.push(&cls), "scheduling class registry full");
}

unsigned int Scheduler::class_count() const {
    const QueueGuard kGuard(*this);
    return classes_.size();
}

SchedulingClass* Scheduler::class_at(unsigned int index) const {
    const QueueGuard kGuard(*this);
    return index < classes_.size() ? classes_.view()[index] : nullptr;
}

void Scheduler::seat(Task& task, bool runnable) {
    const QueueGuard kGuard(*this);
    base::safety::Check(sink_ready(), "scheduler has no sink installed");
    base::safety::Check(!classes_.empty(), "no scheduling class installed");
    if (task.sched_class == nullptr) {
        task.sched_class = classes_.view()[0];
    }
    bool registered = false;
    for (const SchedulingClass* resident : classes_.view()) {
        registered = registered || resident == task.sched_class;
    }
    base::safety::Check(registered, "task points at an unregistered class");
    task.pid   = next_pid_++;
    task.tgid  = task.pid;
    task.state = runnable ? TaskState::kReady : TaskState::kBlocked;
    tasks_.push_back(task);
    if (task.parent != nullptr) {
        offspring_.push_back(task);
    }
    if (runnable) {
        base::safety::Check(task.sched_class->enqueue(task), "ready queue full at seat");
        local().notify_pending = true;
    }
}

Task* Scheduler::pick_next() {
    for (SchedulingClass* cls : classes_.view()) {
        if (Task* next = cls->pick(); next != nullptr) {
            return next;
        }
    }
    return nullptr;
}

Task* Scheduler::current() const {
    const arch::IrqGuard kGuard;
    return local().current;
}

void Scheduler::set_current(Task* task) {
    local().current = task;
}

unsigned long long Scheduler::offspring_count() const {
    const QueueGuard kGuard(*this);
    return offspring_.size();
}

bool Scheduler::nth_group_leader(unsigned long ordinal, unsigned long long& pid) {
    const QueueGuard kGuard(*this);
    unsigned long    seen = 0;
    for (Task* scan = tasks_.head(); scan != nullptr; scan = tasks_.after(*scan)) {
        if (scan->pid != scan->tgid) {
            continue;
        }
        if (seen++ == ordinal) {
            pid = scan->pid;
            return true;
        }
    }
    return false;
}

bool Scheduler::snapshot_by_pid(unsigned long long pid, TaskSnapshot& into) {
    const QueueGuard kGuard(*this);
    for (Task* scan = tasks_.head(); scan != nullptr; scan = tasks_.after(*scan)) {
        if (scan->pid != pid) {
            continue;
        }
        into            = TaskSnapshot{};
        into.pid        = static_cast<long>(scan->pid);
        into.parent_pid = static_cast<long>(scan->parent == nullptr ? 0 : scan->parent->pid);
        into.pgid       = static_cast<long>(scan->pgid);
        into.sid        = static_cast<long>(scan->sid);
        into.state      = state_letter(scan->state);
        base::CopyBytes(into.name, scan->comm, kTaskNameMax);
        into.name[kTaskNameMax - 1] = '\0';
        return true;
    }
    return false;
}

void Scheduler::yield() {
    const arch::IrqGuard kGuard;
    Task* const          kPrev = current();
    base::safety::Check(kPrev != nullptr, "yield called outside any task");
    {
        const QueueGuard kQueue(*this);
        kPrev->state = TaskState::kReady;
        base::safety::Check(kPrev->sched_class->enqueue(*kPrev), "ready queue full at yield");
    }
    rotate_from_interrupt();
}

void Scheduler::rotate_from_interrupt() {
    Task* const kPrev = current();
    Task*       next;
    {
        const QueueGuard kQueue(*this);
        next = pick_next();
        base::safety::Check(next != nullptr, "ready classes lost the outgoing task");
        set_current(next);
        next->state = TaskState::kRunning;
    }
    local().slice_ticks = 0;
    if (next == kPrev) {
        return;
    }
    switch_tasks_(sink_, *kPrev, *next);
    reap_pending();
}

void Scheduler::real_timer(Task& task, unsigned long long now_ns, RealTimerValue& timer,
                           bool replace) {
    const QueueGuard kGuard(*this);
    Task*            owner = &task;
    for (Task* scan = tasks_.head(); scan != nullptr; scan = tasks_.after(*scan)) {
        if (scan->pid == task.tgid) {
            owner = scan;
            break;
        }
    }
    unsigned long long old_value = 0;
    if (owner->real_deadline_ns != 0) {
        old_value = owner->real_deadline_ns > now_ns ? owner->real_deadline_ns - now_ns : 1;
    }
    const auto kOldInterval = owner->real_interval_ns;
    if (replace) {
        owner->real_deadline_ns = timer.value_ns == 0 ? 0 : now_ns + timer.value_ns;
        owner->real_interval_ns = timer.interval_ns;
        if (owner->real_deadline_ns != 0 &&
            owner->real_deadline_ns < next_real_deadline_ns_.load(std::memory_order_relaxed)) {
            next_real_deadline_ns_.store(owner->real_deadline_ns, std::memory_order_release);
        }
    }
    timer = {.value_ns = old_value, .interval_ns = kOldInterval};
}

void Scheduler::expire_real_timers(unsigned long long now_ns) {
    const QueueGuard   kGuard(*this);
    unsigned long long next = ~0ULL;
    for (Task* scan = tasks_.head(); scan != nullptr; scan = tasks_.after(*scan)) {
        if (scan->state == TaskState::kDead || scan->state == TaskState::kZombie ||
            scan->real_deadline_ns == 0) {
            continue;
        }
        if (scan->real_deadline_ns <= now_ns) {
            if (scan->real_interval_ns == 0) {
                scan->real_deadline_ns = 0;
            } else {
                const auto kTail       = (now_ns - scan->real_deadline_ns) % scan->real_interval_ns;
                const auto kDelay      = scan->real_interval_ns - kTail;
                scan->real_deadline_ns = kDelay > ~0ULL - now_ns ? 0 : now_ns + kDelay;
            }
            const auto kGate = scan->signals.gate.guard();
            if (scan->signals.actions[syscall::kSigAlrm - 1].handler != syscall::kSignalIgnore) {
                scan->signals.pending =
                    scan->signals.pending |
                    syscall::SigSet{1ULL << static_cast<unsigned>(syscall::kSigAlrm - 1)};
            }
        }
        if (scan->real_deadline_ns != 0 && scan->real_deadline_ns < next) {
            next = scan->real_deadline_ns;
        }
    }
    next_real_deadline_ns_.store(next, std::memory_order_release);
}

void Scheduler::on_timer_tick(unsigned long long now_ns) {
    if (now_ns >= next_real_deadline_ns_.load(std::memory_order_acquire)) {
        expire_real_timers(now_ns);
    }
    if (current() == nullptr || !preemption_.load(std::memory_order_acquire)) {
        return;
    }
    if (current()->sched_class->wants_rotate(++local().slice_ticks)) {
        local().slice_ticks = 0;
        local().rotate_due  = true;
    }
}

void Scheduler::maybe_preempt() {
    if (!local().rotate_due || current() == nullptr ||
        !preemption_.load(std::memory_order_acquire) || local().preempt_depth != 0 ||
        DefaultSpinLedger().depth() != 0 || local().queue_depth != 0) {
        return;
    }
    const arch::IrqGuard kGuard;
    local().rotate_due = false;
    {
        const QueueGuard kQueue(*this);
        current()->state = TaskState::kReady;
        base::safety::Check(current()->sched_class->enqueue(*current()),
                            "ready queue full at preemption");
    }
    rotate_from_interrupt();
}

void Scheduler::set_preemption(bool enabled) {
    const arch::IrqGuard kGuard;
    preemption_.store(enabled, std::memory_order_release);
    local().slice_ticks = 0;
    local().rotate_due  = false;
}

void Scheduler::preempt_disable() {
    const arch::IrqGuard kGuard;
    ++local().preempt_depth;
}

void Scheduler::preempt_enable() {
    const arch::IrqGuard kGuard;
    base::safety::Check(local().preempt_depth != 0, "preempt guard underflow");
    --local().preempt_depth;
}

void Scheduler::exit_current(long long code) {
    const arch::IrqGuard kGuard;
    Task* const          kPrev = current();
    base::safety::Check(kPrev != nullptr, "exit called outside any task");
    futex::ClearTidWord(*kPrev);
    kPrev->files.reset();
    reap_pending();
    Task* next;
    {
        const QueueGuard kQueue(*this);
        kPrev->exit_code = code;
        detach_offspring(*kPrev);
        const bool kThreadMember = kPrev->pid != kPrev->tgid;
        if (kPrev->parent != nullptr && !kThreadMember) {
            kPrev->state = TaskState::kZombie;
            SendSignal(*kPrev->parent, cinux::syscall::kSigChld);
            if (waiters_.remove(*kPrev->parent)) {
                unblock(*kPrev->parent);
            }
        } else {
            tasks_.remove(*kPrev);
            if (kPrev->parent != nullptr) {
                offspring_.remove(*kPrev);
            }
            kPrev->state         = TaskState::kDead;
            local().pending_reap = kPrev;
        }
        next = pick_next();
        set_current(next);
        if (next != nullptr) {
            next->state = TaskState::kRunning;
        }
        local().slice_ticks = 0;
    }
    if (next != nullptr) {
        switch_tasks_(sink_, *kPrev, *next);
    } else {
        return_to_main_(sink_, *kPrev);
    }
}

Task* Scheduler::take_zombie_child(Task& watcher, long long wanted, bool& has_eligible) {
    has_eligible        = false;
    const bool kAny     = wanted == -1;
    const bool kGrouped = wanted <= 0 && !kAny;
    const auto kGroup   = wanted < -1 ? static_cast<unsigned long long>(-wanted) : watcher.pgid;
    Task*      match    = nullptr;
    for (Task* scan = offspring_.head(); scan != nullptr; scan = offspring_.after(*scan)) {
        if (scan->parent != &watcher || scan->pid != scan->tgid) {
            continue;
        }
        const bool kEligible =
            kAny || (kGrouped ? scan->pgid == kGroup : std::cmp_equal(scan->pid, wanted));
        has_eligible = has_eligible || kEligible;
        if (kEligible && scan->state == TaskState::kZombie) {
            match = scan;
            break;
        }
    }
    if (match != nullptr) {
        offspring_.remove(*match);
        tasks_.remove(*match);
    }
    return match;
}

void Scheduler::detach_offspring(Task& dying) {
    Task* scan = offspring_.head();
    while (scan != nullptr) {
        Task* const kNext = offspring_.after(*scan);
        if (scan->parent == &dying) {
            offspring_.remove(*scan);
            scan->parent = nullptr;
            if (scan->state == TaskState::kZombie) {
                tasks_.remove(*scan);
                if (__atomic_load_n(&scan->on_cpu, __ATOMIC_ACQUIRE) == -1) {
                    reclaim_(sink_, *scan);
                } else {
                    retired_.push_back(*scan);
                }
            }
        }
        scan = kNext;
    }
}

bool Scheduler::wait_child(long long wanted, bool no_hang, ChildReceipt& receipt) {
    for (;;) {
        const arch::IrqGuard kIrq;
        Task* const          kSelf = current();
        base::safety::Check(kSelf != nullptr, "wait called outside any task");
        Task* match;
        {
            const QueueGuard kQueue(*this);
            bool             has_child = false;
            match                      = take_zombie_child(*kSelf, wanted, has_child);
            if (match == nullptr) {
                if (!has_child) {
                    return false;
                }
                if (no_hang) {
                    receipt.pid = 0;
                    return true;
                }
                base::safety::Check(DefaultSpinLedger().depth() == 0,
                                    "task parked while holding a spinlock");
                waiters_.push_back(*kSelf);
                kSelf->state = TaskState::kBlocked;
            }
        }
        if (match != nullptr) {
            receipt.pid  = match->pid;
            receipt.code = match->exit_code;
            reclaim_(sink_, *match);
            return true;
        }
        schedule_blocked();
    }
}

void Scheduler::prepare_to_wait() {
    const QueueGuard kQueue(*this);
    base::safety::Check(current() != nullptr, "block called outside any task");
    current()->state = TaskState::kBlocked;
}

void Scheduler::block_current() {
    const arch::IrqGuard kGuard;
    base::safety::Check(DefaultSpinLedger().depth() == 0, "task parked while holding a spinlock");
    park_current();
}

void Scheduler::park_current() {
    prepare_to_wait();
    schedule_blocked();
}

void Scheduler::schedule_blocked() {
    const arch::IrqGuard kIrq;
    if (local().no_reschedule_depth != 0) {
        return;
    }
    base::safety::Check(DefaultSpinLedger().depth() == 0, "task parked while holding a spinlock");
    Task* const kPrev = current();
    Task*       next;
    {
        const QueueGuard kQueue(*this);
        next = pick_next();
        set_current(next);
        if (next != nullptr) {
            next->state = TaskState::kRunning;
        }
        local().slice_ticks = 0;
    }
    if (next == kPrev) {
        return;
    }
    if (next != nullptr) {
        switch_tasks_(sink_, *kPrev, *next);
        reap_pending();
    } else {
        return_to_main_(sink_, *kPrev);
    }
}

void Scheduler::unblock(Task& task) {
    const QueueGuard kGuard(*this);
    if (task.state != TaskState::kBlocked) {
        return;
    }
    task.state = TaskState::kReady;
    base::safety::Check(task.sched_class->enqueue(task), "ready queue full at unblock");
    local().notify_pending = true;
}

void Scheduler::stop_current() {
    const arch::IrqGuard kGuard;
    base::safety::Check(current() != nullptr, "stop called outside any task");
    base::safety::Check(DefaultSpinLedger().depth() == 0, "task stopped while holding a spinlock");
    {
        const QueueGuard kQueue(*this);
        current()->state = TaskState::kStopped;
    }
    schedule_blocked();
}

void Scheduler::resume(Task& task) {
    const QueueGuard kGuard(*this);
    base::safety::Check(task.state == TaskState::kStopped, "resume of a task that is not stopped");
    task.state = TaskState::kReady;
    base::safety::Check(task.sched_class->enqueue(task), "ready queue full at resume");
    local().notify_pending = true;
}

bool Scheduler::withdraw(Task& task) {
    const QueueGuard kGuard(*this);
    if (task.state != TaskState::kReady) {
        return false;
    }
    base::safety::Check(task.sched_class->remove(task),
                        "ready queue lost a task the stop signal named");
    task.state = TaskState::kStopped;
    return true;
}

void Scheduler::run_until_done() {
    const arch::IrqGuard kGuard;
    base::safety::Check(current() == nullptr, "run_until_done called from a task");
    base::safety::Check(sink_ready(), "scheduler has no sink installed");
    for (;;) {
        Task* next;
        {
            const QueueGuard kQueue(*this);
            next = pick_next();
            if (next == nullptr) {
                return;
            }
            set_current(next);
            next->state         = TaskState::kRunning;
            local().slice_ticks = 0;
        }
        enter_from_main_(sink_, *next);
        reap_pending();
    }
}

void Scheduler::reap_pending() {
    Task* const kDead = local().pending_reap;
    if (kDead != nullptr && __atomic_load_n(&kDead->on_cpu, __ATOMIC_ACQUIRE) == -1) {
        local().pending_reap = nullptr;
        reclaim_(sink_, *kDead);
    }
    for (;;) {
        Task* ready = nullptr;
        {
            const QueueGuard kQueue(*this);
            for (Task* scan = retired_.head(); scan != nullptr; scan = retired_.after(*scan)) {
                if (__atomic_load_n(&scan->on_cpu, __ATOMIC_ACQUIRE) == -1) {
                    ready = scan;
                    break;
                }
            }
            if (ready != nullptr) {
                retired_.remove(*ready);
            }
        }
        if (ready == nullptr) {
            return;
        }
        reclaim_(sink_, *ready);
    }
}

}  // namespace cinux::proc
