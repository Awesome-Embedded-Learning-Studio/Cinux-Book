#include "cinux/assert.hpp"
#include "cinux/bit_ops/bitmask.hpp"
#include "cinux/ptr.hpp"
#include "framework_kernel.hpp"
#include "kernel/arch/x86_64/irq_guard.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/proc/proc_config.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/sync.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/time/tick.hpp"
#include "test_assert.hpp"
#include "test_case.hpp"  // NOLINT(misc-include-cleaner) consumed by the TEST() macro body

namespace {

constexpr unsigned long long kSentinelR12  = 0xA11CE12C0DE0001ULL;
constexpr unsigned long long kSentinelR13  = 0xA11CE12C0DE0002ULL;
constexpr unsigned long long kSentinelR14  = 0xA11CE12C0DE0003ULL;
constexpr unsigned long long kSentinelR15  = 0xA11CE12C0DE0004ULL;
constexpr unsigned long long kSentinelXmm0 = 0xFEEDFACE0DDBA115ULL;

unsigned int g_order_index = 0;
unsigned int g_order[8]    = {};

bool saw_strict_alternation() {
    if (g_order_index != 6) {
        return false;
    }
    for (unsigned int index = 0; index + 1 < g_order_index; ++index) {
        if (g_order[index] == g_order[index + 1]) {
            return false;
        }
    }
    return true;
}

void ping_pong_task() {
    auto& scheduler = cinux::proc::Scheduler::self();
    for (unsigned int round = 0; round < 3; ++round) {
        if (g_order_index < 8) {
            g_order[g_order_index++] = static_cast<unsigned int>(scheduler.current()->tid);
        }
        scheduler.yield();
    }
}

void probe_task() {
    auto&              scheduler = cinux::proc::Scheduler::self();
    // NOLINTBEGIN(misc-const-correctness) written by the asm output below
    unsigned long long r12       = 0;
    unsigned long long r13       = 0;
    unsigned long long r14       = 0;
    unsigned long long r15       = 0;
    unsigned long long xmm0      = 0;
    // NOLINTEND(misc-const-correctness)
    __asm__ volatile(
        "movabsq %[c12], %%r12\n\t"
        "movabsq %[c13], %%r13\n\t"
        "movabsq %[c14], %%r14\n\t"
        "movabsq %[c15], %%r15\n\t"
        "movq %[cx0], %%xmm0\n\t"
        :
        : [c12] "i"(kSentinelR12), [c13] "i"(kSentinelR13), [c14] "i"(kSentinelR14),
          [c15] "i"(kSentinelR15), [cx0] "m"(kSentinelXmm0)
        : "r12", "r13", "r14", "r15", "xmm0", "memory");
    cinux::proc::Scheduler::self().yield();
    __asm__ volatile(
        "movq %%xmm0, %[ox0]\n\t"
        "movq %%r12, %[o12]\n\t"
        "movq %%r13, %[o13]\n\t"
        "movq %%r14, %[o14]\n\t"
        "movq %%r15, %[o15]\n\t"
        : [ox0] "=m"(xmm0), [o12] "=m"(r12), [o13] "=m"(r13), [o14] "=m"(r14), [o15] "=m"(r15)
        :
        : "r12", "r13", "r14", "r15", "xmm0", "memory");
    ASSERT_TRUE(xmm0 == kSentinelXmm0);
    ASSERT_TRUE(r12 == kSentinelR12);
    ASSERT_TRUE(r13 == kSentinelR13);
    ASSERT_TRUE(r14 == kSentinelR14);
    ASSERT_TRUE(r15 == kSentinelR15);
    const unsigned long long kMagic =
        *cinux::base::PtrAt<const unsigned long long>(scheduler.current()->stack_base);
    ASSERT_TRUE(kMagic == cinux::proc::kStackMagic);
}

void instant_exit_task() {}

cinux::proc::Semaphore g_blocking_gate;
bool                   g_blocking_task_irq_enabled = false;

bool irq_enabled(unsigned long long snapshot) {
    using cinux::base::bit::BitMask;
    using cinux::base::bit::MaskBit;
    return BitMask<unsigned long long>{snapshot}.has(MaskBit<unsigned long long>(9));
}

void blocking_task() {
    g_blocking_gate.wait();
    const unsigned long long kSnapshot = cinux::arch::SaveAndDisableIrq();
    g_blocking_task_irq_enabled        = irq_enabled(kSnapshot);
    cinux::arch::RestoreIrq(kSnapshot);
}

void check_blocking_round_trip(bool main_irq_enabled) {
    auto& scheduler                  = cinux::proc::Scheduler::self();
    g_blocking_task_irq_enabled      = false;
    const unsigned long long kBefore = cinux::arch::SaveAndDisableIrq();
    cinux::arch::RestoreIrq(kBefore);
    auto* const kTask =
        cinux::proc::TaskBuilder{}.set_entry(blocking_task).set_name("blocked").build();
    cinux::base::safety::Check(kTask != nullptr, "blocking test task failed to build");
    scheduler.seat(*kTask);
    scheduler.run_until_done();
    const unsigned long long kAfterBlock = cinux::arch::SaveAndDisableIrq();
    const bool               kParked     = kTask->state == cinux::proc::TaskState::kBlocked;
    cinux::arch::RestoreIrq(kBefore);
    g_blocking_gate.post();
    scheduler.run_until_done();
    const unsigned long long kAfterExit = cinux::arch::SaveAndDisableIrq();
    cinux::arch::RestoreIrq(kBefore);
    ASSERT_TRUE(irq_enabled(kBefore) == main_irq_enabled);
    ASSERT_TRUE(kParked);
    ASSERT_TRUE(irq_enabled(kAfterBlock) == main_irq_enabled);
    ASSERT_TRUE(irq_enabled(kAfterExit) == main_irq_enabled);
    ASSERT_TRUE(g_blocking_task_irq_enabled);
    ASSERT_TRUE(scheduler.current() == nullptr);
}

struct VisitLog {
    unsigned int tids[64];
    unsigned int len;
};

VisitLog g_log_a{};
VisitLog g_log_b{};

VisitLog* log_for(const char* name) {
    return name[1] == 'A' ? &g_log_a : &g_log_b;
}

void preempt_probe_task() {
    auto&                    scheduler = cinux::proc::Scheduler::self();
    VisitLog*                log       = log_for(scheduler.current()->name);
    const unsigned long long kDeadline = cinux::time::Tick::self().since_boot() + 50;
    while (cinux::time::Tick::self().since_boot() < kDeadline) {
        if (log->len < 64) {
            log->tids[log->len++] = static_cast<unsigned int>(scheduler.current()->tid);
        }
    }
}

void seat_built(cinux::proc::Task* task) {
    cinux::base::safety::Check(task != nullptr, "test task failed to build");
    cinux::proc::Scheduler::self().seat(*task);
}

struct Market {
    cinux::proc::Mutex     door;
    cinux::proc::Semaphore free_slots;
    cinux::proc::Semaphore goods;
    unsigned int           buffer[4]    = {};
    unsigned int           put_index    = 0;
    unsigned int           take_index   = 0;
    unsigned long long     produced_sum = 0;
    unsigned long long     consumed_sum = 0;

    Market() = default;
};

Market g_market{};

void producer_task() {
    for (unsigned int item = 1; item <= 8; ++item) {
        g_market.free_slots.wait();
        {
            const cinux::proc::MutexGuard kDoor(g_market.door);
            g_market.buffer[g_market.put_index % 4] = item;
            g_market.put_index++;
            g_market.produced_sum += item;
        }
        g_market.goods.post();
    }
}

void consumer_task() {
    for (unsigned int round = 0; round < 8; ++round) {
        g_market.goods.wait();
        unsigned int item = 0;
        {
            const cinux::proc::MutexGuard kDoor(g_market.door);
            item = g_market.buffer[g_market.take_index % 4];
            g_market.take_index++;
            g_market.consumed_sum += item;
        }
        g_market.free_slots.post();
    }
}

}  // namespace

TEST("sched: two tasks ping-pong through cooperative yield") {
    auto& scheduler = cinux::proc::Scheduler::self();
    g_order_index   = 0;
    seat_built(cinux::proc::TaskBuilder{}.set_entry(ping_pong_task).set_name("ping").build());
    seat_built(cinux::proc::TaskBuilder{}.set_entry(ping_pong_task).set_name("pong").build());
    scheduler.run_until_done();
    ASSERT_TRUE(saw_strict_alternation());
}

TEST("sched: callee-saved registers and xmm0 survive switches") {
    auto& scheduler = cinux::proc::Scheduler::self();
    seat_built(cinux::proc::TaskBuilder{}.set_entry(probe_task).set_name("probe").build());
    seat_built(cinux::proc::TaskBuilder{}.set_entry(ping_pong_task).set_name("partner").build());
    scheduler.run_until_done();
}

TEST("sched: spawn-exit cycles return pages to the pmm") {
    auto&               scheduler = cinux::proc::Scheduler::self();
    auto&               ledger    = cinux::mm::Pmm::self();
    const unsigned long kBefore   = ledger.free_page_count();
    for (unsigned int round = 0; round < 8; ++round) {
        seat_built(
            cinux::proc::TaskBuilder{}.set_entry(instant_exit_task).set_name("one-shot").build());
    }
    scheduler.run_until_done();
    ASSERT_TRUE(ledger.free_page_count() == kBefore);
}

TEST("sched: blocking and exiting restore main's enabled interrupts") {
    cinux::arch::RestoreIrq(cinux::base::bit::MaskBit<unsigned long long>(9).raw);
    check_blocking_round_trip(true);
}

TEST("sched: blocking and exiting preserve main's disabled interrupts") {
    const cinux::arch::IrqGuard kGuard;
    check_blocking_round_trip(false);
}

TEST("sched: the clock rotates tasks that never yield") {
    auto& scheduler = cinux::proc::Scheduler::self();
    g_log_a         = VisitLog{};
    g_log_b         = VisitLog{};
    seat_built(cinux::proc::TaskBuilder{}.set_entry(preempt_probe_task).set_name("pA").build());
    seat_built(cinux::proc::TaskBuilder{}.set_entry(preempt_probe_task).set_name("pB").build());
    scheduler.set_preemption(true);
    scheduler.run_until_done();
    scheduler.set_preemption(false);
    ASSERT_TRUE(g_log_a.len > 0);
    ASSERT_TRUE(g_log_b.len > 0);
    ASSERT_TRUE(g_log_a.tids[0] != g_log_b.tids[0]);
}

TEST("sched: producer and consumer close the loop through locks") {
    auto& scheduler = cinux::proc::Scheduler::self();
    for (unsigned int slot = 0; slot < 4; ++slot) {
        g_market.free_slots.post();
    }
    seat_built(cinux::proc::TaskBuilder{}.set_entry(producer_task).set_name("producer").build());
    seat_built(cinux::proc::TaskBuilder{}.set_entry(consumer_task).set_name("consumer").build());
    scheduler.set_preemption(true);
    scheduler.run_until_done();
    scheduler.set_preemption(false);
    ASSERT_TRUE(g_market.produced_sum == 36ULL);
    ASSERT_TRUE(g_market.consumed_sum == 36ULL);
    ASSERT_TRUE(g_market.put_index == 8);
    ASSERT_TRUE(g_market.take_index == 8);
}

TEST("sched: the irq guard freezes the clock until its outermost exit") {
    const unsigned long long kBefore = cinux::time::Tick::self().since_boot();
    {
        const cinux::arch::IrqGuard kGuard;
        {
            const cinux::arch::IrqGuard kNested;
            ASSERT_TRUE(cinux::time::Tick::self().since_boot() == kBefore);
        }
        volatile unsigned long spins = 0;
        for (unsigned long index = 0; index < 30000000UL; ++index) {
            spins = spins + 1;
        }
        ASSERT_TRUE(spins == 30000000UL);
        ASSERT_TRUE(cinux::time::Tick::self().since_boot() == kBefore);
    }
    const unsigned long long kResumeDeadline = kBefore + 5;
    while (cinux::time::Tick::self().since_boot() < kResumeDeadline) {
    }
    ASSERT_TRUE(cinux::time::Tick::self().since_boot() > kBefore);
}
