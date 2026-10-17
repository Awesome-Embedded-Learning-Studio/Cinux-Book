#include "cinux/addr.hpp"
#include "cinux/assert.hpp"
#include "cinux/bit_ops/bitmask.hpp"
#include "cinux/literal_types.hpp"
#include "cinux/ptr.hpp"
#include "cinux/result.hpp"
#include "cinux/shared.hpp"
#include "framework_kernel.hpp"
#include "kernel/arch/x86_64/context.hpp"
#include "kernel/arch/x86_64/irq_guard.hpp"
#include "kernel/arch/x86_64/isr.hpp"
#include "kernel/arch/x86_64/registers.hpp"
#include "kernel/mm/address_space.hpp"
#include "kernel/mm/cow.hpp"
#include "kernel/mm/layout.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/vma.hpp"
#include "kernel/mm/vmm.hpp"
#include "kernel/proc/clone.hpp"
#include "kernel/proc/fork.hpp"
#include "kernel/proc/futex.hpp"
#include "kernel/proc/proc_config.hpp"
#include "kernel/proc/sched_class.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/sync.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/time/tick.hpp"
#include "test/mock/file_refs.hpp"
#include "test_assert.hpp"
#include "test_case.hpp"  // NOLINT(misc-include-cleaner) consumed by the TEST() macro body

extern "C" void ForkChildEntry();

using cinux::base::operator""_KiB;

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
            g_order[g_order_index++] = static_cast<unsigned int>(scheduler.current()->pid);
        }
        scheduler.yield();
    }
}

void probe_task() {
    const auto&        scheduler = cinux::proc::Scheduler::self();
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

cinux::test::FileRefs g_file_refs;

void leave_files_open_task() {
    const auto& files = cinux::proc::Scheduler::self().current()->files;
    ASSERT_TRUE(files->alloc(&cinux::test::FileRefs::kOps, &g_file_refs, {}).ok());
    ASSERT_TRUE(files->alloc(&cinux::test::FileRefs::kOps, &g_file_refs, {}).ok());
}

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
    unsigned int pids[64];
    unsigned int len;
};

VisitLog g_log_a{};
VisitLog g_log_b{};

VisitLog* log_for(const char* name) {
    return name[1] == 'A' ? &g_log_a : &g_log_b;
}

void preempt_probe_task() {
    const auto&              scheduler = cinux::proc::Scheduler::self();
    VisitLog*                log       = log_for(scheduler.current()->name);
    const unsigned long long kDeadline = cinux::time::Tick::self().since_boot() + 50;
    while (cinux::time::Tick::self().since_boot() < kDeadline) {
        if (log->len < 64) {
            log->pids[log->len++] = static_cast<unsigned int>(scheduler.current()->pid);
        }
    }
}

void seat_built(cinux::proc::Task* task) {
    cinux::base::safety::Check(task != nullptr, "test task failed to build");
    cinux::proc::Scheduler::self().seat(*task);
}

VisitLog g_order_roll{};

void visit_once_task() {
    if (g_order_roll.len < 64) {
        g_order_roll.pids[g_order_roll.len++] =
            static_cast<unsigned int>(cinux::proc::Scheduler::self().current()->pid);
    }
    cinux::proc::Scheduler::self().exit_current();
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

cinux::proc::ChildReceipt g_receipt{};
bool                      g_wait_ok       = false;
unsigned long long        g_child_pid     = 0;
unsigned int              g_grand_reaped  = 0;
unsigned int              g_nested_reaped = 0;

void coded_child_task() {
    cinux::proc::Scheduler::self().exit_current(42);
}

void await_nested_child(void (*entry)(), long long expected_code) {
    auto&       scheduler = cinux::proc::Scheduler::self();
    auto* const kChild    = cinux::proc::TaskBuilder{}
                                .set_entry(entry)
                                .set_name("nested_waiter")
                                .set_parent(*scheduler.current())
                                .build();
    cinux::base::safety::Check(kChild != nullptr, "nested wait child failed to build");
    scheduler.seat(*kChild);
    const auto                kPid = kChild->pid;
    cinux::proc::ChildReceipt receipt{};
    ASSERT_TRUE(scheduler.wait_child(static_cast<long long>(kPid), false, receipt));
    ASSERT_EQ(receipt.pid, kPid);
    ASSERT_EQ(receipt.code, expected_code);
    g_nested_reaped++;
    scheduler.exit_current(expected_code + 1);
}

void nested_mapper_task() {
    await_nested_child(coded_child_task, 42);
}

void nested_shell_task() {
    await_nested_child(nested_mapper_task, 43);
}

void nested_init_task() {
    await_nested_child(nested_shell_task, 44);
}

void waiting_parent_task() {
    auto&       scheduler = cinux::proc::Scheduler::self();
    auto* const kChild    = cinux::proc::TaskBuilder{}
                                .set_entry(coded_child_task)
                                .set_name("coded_child")
                                .set_parent(*scheduler.current())
                                .build();
    cinux::base::safety::Check(kChild != nullptr, "wait test child failed to build");
    scheduler.seat(*kChild);
    g_wait_ok = scheduler.wait_child(-1, false, g_receipt);
}

void returning_child_task() {}

void late_waiter_parent_task() {
    auto&       scheduler = cinux::proc::Scheduler::self();
    auto* const kChild    = cinux::proc::TaskBuilder{}
                                .set_entry(returning_child_task)
                                .set_name("early_child")
                                .set_parent(*scheduler.current())
                                .build();
    cinux::base::safety::Check(kChild != nullptr, "late wait child failed to build");
    scheduler.seat(*kChild);
    g_child_pid = kChild->pid;
    for (unsigned int round = 0; round < 4; ++round) {
        scheduler.yield();
    }
    g_wait_ok = scheduler.wait_child(static_cast<long long>(g_child_pid), false, g_receipt);
}

void childless_waiter_task() {
    g_wait_ok = cinux::proc::Scheduler::self().wait_child(-1, false, g_receipt);
}

void slow_orphan_child_task() {
    for (unsigned int round = 0; round < 4; ++round) {
        cinux::proc::Scheduler::self().yield();
    }
}

void doomed_parent_task() {
    auto&       scheduler = cinux::proc::Scheduler::self();
    auto* const kChild    = cinux::proc::TaskBuilder{}
                                .set_entry(slow_orphan_child_task)
                                .set_name("orphan_child")
                                .set_parent(*scheduler.current())
                                .build();
    cinux::base::safety::Check(kChild != nullptr, "orphan child failed to build");
    scheduler.seat(*kChild);
}

void seed_clone_fixture(const cinux::mm::AddressSpace& parent, const unsigned long spots[3],
                        cinux::base::PhysAddr frames[2]) {
    auto& ledger = cinux::mm::Pmm::self();
    for (unsigned int which = 0; which < 2; ++which) {
        frames[which] = ledger.allocate_page();
        cinux::base::safety::Check(frames[which] != cinux::base::PhysAddr{},
                                   "clone fixture out of frames");
    }
    cinux::base::safety::Check(parent.map_user(spots[0], frames[0].raw), "fixture map a");
    cinux::base::safety::Check(parent.map_user(spots[1], frames[0].raw), "fixture map b");
    cinux::base::safety::Check(parent.map_user(spots[2], frames[1].raw), "fixture map c");
}

void sibling_parent_task() {
    auto&       scheduler = cinux::proc::Scheduler::self();
    auto* const kChild    = cinux::proc::TaskBuilder{}
                                .set_entry(returning_child_task)
                                .set_name("sibling_child")
                                .set_parent(*scheduler.current())
                                .build();
    cinux::base::safety::Check(kChild != nullptr, "sibling child failed to build");
    scheduler.seat(*kChild);
    for (unsigned int round = 0; round < 6; ++round) {
        scheduler.yield();
    }
    g_wait_ok = scheduler.wait_child(-1, false, g_receipt);
}

void grandmother_task() {
    auto&       scheduler = cinux::proc::Scheduler::self();
    auto* const kDoomed   = cinux::proc::TaskBuilder{}
                                .set_entry(doomed_parent_task)
                                .set_name("doomed_sibling")
                                .set_parent(*scheduler.current())
                                .build();
    auto* const kWaiter   = cinux::proc::TaskBuilder{}
                                .set_entry(sibling_parent_task)
                                .set_name("waiting_sibling")
                                .set_parent(*scheduler.current())
                                .build();
    cinux::base::safety::Check(kDoomed != nullptr && kWaiter != nullptr,
                               "siblings failed to build");
    scheduler.seat(*kDoomed);
    scheduler.seat(*kWaiter);
    for (unsigned int round = 0; round < 12; ++round) {
        scheduler.yield();
    }
    cinux::proc::ChildReceipt gone{};
    while (scheduler.wait_child(-1, false, gone)) {
        g_grand_reaped++;
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
    const auto&         ledger    = cinux::mm::Pmm::self();
    const unsigned long kBefore   = ledger.free_page_count();
    for (unsigned int round = 0; round < 8; ++round) {
        seat_built(
            cinux::proc::TaskBuilder{}.set_entry(instant_exit_task).set_name("one-shot").build());
    }
    scheduler.run_until_done();
    ASSERT_TRUE(ledger.free_page_count() == kBefore);
}

TEST("sched: task reclamation closes files left open at exit") {
    g_file_refs = {};
    seat_built(
        cinux::proc::TaskBuilder{}.set_entry(leave_files_open_task).set_name("files").build());
    cinux::proc::Scheduler::self().run_until_done();
    ASSERT_TRUE(g_file_refs.retained == 2);
    ASSERT_TRUE(g_file_refs.released == 2);
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
    ASSERT_TRUE(g_log_a.pids[0] != g_log_b.pids[0]);
}

TEST("sched: the priority class sends its smallest weight in first") {
    auto& scheduler                  = cinux::proc::Scheduler::self();
    g_order_roll                     = VisitLog{};
    cinux::proc::Task* const kLate   = cinux::proc::TaskBuilder{}
                                           .set_entry(visit_once_task)
                                           .set_name("p-low")
                                           .set_class(cinux::proc::PriorityClass())
                                           .set_priority(9)
                                           .build();
    cinux::proc::Task* const kUrgent = cinux::proc::TaskBuilder{}
                                           .set_entry(visit_once_task)
                                           .set_name("p-urgent")
                                           .set_class(cinux::proc::PriorityClass())
                                           .set_priority(1)
                                           .build();
    seat_built(kLate);
    seat_built(kUrgent);
    scheduler.run_until_done();
    ASSERT_TRUE(g_order_roll.len == 2);
    ASSERT_TRUE(g_order_roll.pids[0] == kUrgent->pid);
    ASSERT_TRUE(g_order_roll.pids[1] == kLate->pid);
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

TEST("sched: a parent's wait reaps the child and carries its exit word") {
    g_receipt = cinux::proc::ChildReceipt{};
    g_wait_ok = false;
    seat_built(
        cinux::proc::TaskBuilder{}.set_entry(waiting_parent_task).set_name("parent").build());
    cinux::proc::Scheduler::self().run_until_done();
    ASSERT_TRUE(g_wait_ok);
    ASSERT_TRUE(g_receipt.code == 42);
    ASSERT_TRUE(g_receipt.pid != 0);
}

TEST("sched: a child that died before the wait still answers it") {
    g_receipt   = cinux::proc::ChildReceipt{};
    g_wait_ok   = false;
    g_child_pid = 0;
    seat_built(
        cinux::proc::TaskBuilder{}.set_entry(late_waiter_parent_task).set_name("late").build());
    cinux::proc::Scheduler::self().run_until_done();
    ASSERT_TRUE(g_wait_ok);
    ASSERT_TRUE(g_receipt.code == 0);
    ASSERT_TRUE(g_receipt.pid == g_child_pid);
}

TEST("sched: waiting with no child refuses instead of parking") {
    g_wait_ok = true;
    seat_built(
        cinux::proc::TaskBuilder{}.set_entry(childless_waiter_task).set_name("childless").build());
    cinux::proc::Scheduler::self().run_until_done();
    ASSERT_TRUE(!g_wait_ok);
}

TEST("sched: waited spawn-exit cycles also return pages to the pmm") {
    auto&               scheduler = cinux::proc::Scheduler::self();
    const auto&         ledger    = cinux::mm::Pmm::self();
    const unsigned long kBefore   = ledger.free_page_count();
    for (unsigned int round = 0; round < 4; ++round) {
        seat_built(
            cinux::proc::TaskBuilder{}.set_entry(waiting_parent_task).set_name("waiter").build());
    }
    scheduler.run_until_done();
    ASSERT_TRUE(ledger.free_page_count() == kBefore);
}

TEST("sched: a parent dying first releases its child to a parentless life") {
    auto&               scheduler = cinux::proc::Scheduler::self();
    const auto&         ledger    = cinux::mm::Pmm::self();
    const unsigned long kBefore   = ledger.free_page_count();
    for (unsigned int round = 0; round < 4; ++round) {
        seat_built(
            cinux::proc::TaskBuilder{}.set_entry(doomed_parent_task).set_name("doomed").build());
    }
    scheduler.run_until_done();
    ASSERT_TRUE(ledger.free_page_count() == kBefore);
}

TEST("sched: orphan detach leaves nothing behind on the offspring roll") {
    auto&                    scheduler = cinux::proc::Scheduler::self();
    const unsigned long long kBaseline = scheduler.offspring_count();
    for (unsigned int round = 0; round < 8; ++round) {
        seat_built(
            cinux::proc::TaskBuilder{}.set_entry(doomed_parent_task).set_name("doomed").build());
    }
    scheduler.run_until_done();
    ASSERT_TRUE(scheduler.offspring_count() == kBaseline);
}

TEST("sched: one family's deaths leave another family's waits intact") {
    auto&               scheduler = cinux::proc::Scheduler::self();
    const auto&         ledger    = cinux::mm::Pmm::self();
    const unsigned long kBefore   = ledger.free_page_count();
    g_receipt                     = cinux::proc::ChildReceipt{};
    g_wait_ok                     = false;
    g_grand_reaped                = 0;
    seat_built(cinux::proc::TaskBuilder{}.set_entry(grandmother_task).set_name("grand").build());
    scheduler.run_until_done();
    ASSERT_TRUE(g_wait_ok);
    ASSERT_TRUE(g_grand_reaped == 2);
    ASSERT_TRUE(ledger.free_page_count() == kBefore);
}

TEST("fork: the clone mirrors every parent page") {
    const auto* const kWarm = new cinux::mm::AddressSpace{};
    delete kWarm;
    const auto&             ledger  = cinux::mm::Pmm::self();
    const unsigned long     kBefore = ledger.free_page_count();
    cinux::mm::AddressSpace parent;
    ASSERT_TRUE(parent.init());
    const unsigned long kSpots[3] = {
        cinux::mm::kUserNullTop + 64_KiB,
        cinux::mm::kUserNullTop + 68_KiB,
        cinux::mm::kUserBrkMmapSplit + 32_KiB,
    };
    cinux::base::PhysAddr frames[2] = {};
    seed_clone_fixture(parent, kSpots, frames);
    cinux::mm::AddressSpace* const kChild = cinux::mm::cow::CloneSharedSpace(parent);
    ASSERT_TRUE(kChild != nullptr);
    ASSERT_EQ(kChild->translate(kSpots[0]), frames[0].raw);
    ASSERT_EQ(kChild->translate(kSpots[1]), frames[0].raw);
    ASSERT_EQ(kChild->translate(kSpots[2]), frames[1].raw);
    kChild->teardown();
    delete kChild;
    parent.teardown();
    ASSERT_EQ(ledger.free_page_count(), kBefore);
}

TEST("fork: a write on one side leaves the other side's view alone") {
    const auto* const kWarm = new cinux::mm::AddressSpace{};
    delete kWarm;
    const auto&             ledger  = cinux::mm::Pmm::self();
    const unsigned long     kBefore = ledger.free_page_count();
    cinux::mm::AddressSpace parent;
    ASSERT_TRUE(parent.init());
    const unsigned long kSpots[3] = {
        cinux::mm::kUserNullTop + 72_KiB,
        cinux::mm::kUserNullTop + 76_KiB,
        cinux::mm::kUserBrkMmapSplit + 36_KiB,
    };
    cinux::base::PhysAddr frames[2] = {};
    seed_clone_fixture(parent, kSpots, frames);
    cinux::mm::AddressSpace* const kChild = cinux::mm::cow::CloneSharedSpace(parent);
    ASSERT_TRUE(kChild != nullptr);
    kChild->activate();
    ASSERT_TRUE(cinux::mm::cow::TryResolvePageFault(
        kSpots[0], cinux::arch::isr::kFaultWasWrite | cinux::arch::isr::kFaultFromUser));
    ASSERT_TRUE(kChild->translate(kSpots[0]) != frames[0].raw);
    ASSERT_EQ(kChild->translate(kSpots[1]), frames[0].raw);
    ASSERT_EQ(parent.translate(kSpots[0]), frames[0].raw);
    cinux::arch::LoadCr3(cinux::mm::KernelPageRoot());
    kChild->teardown();
    delete kChild;
    parent.teardown();
    ASSERT_EQ(ledger.free_page_count(), kBefore);
}

namespace {

constexpr unsigned long long kForkUserR12 = 0xF012000000000001ULL;
constexpr unsigned long long kForkUserR13 = 0xF013000000000002ULL;
constexpr unsigned long long kForkUserR14 = 0xF014000000000003ULL;
constexpr unsigned long long kForkUserR15 = 0xF015000000000004ULL;
constexpr unsigned long long kForkUserRsp = 0x0000000000005000ULL;
constexpr unsigned long long kForkUserRip = 0x0000000000400016ULL;

cinux::base::Shared<cinux::mm::AddressSpace> g_fork_world;
cinux::base::Shared<cinux::mm::VmaStore>     g_fork_ranges;
cinux::syscall::SyscallFrame                 g_fork_frame{};
bool                                         g_fork_built = false;

void stage_callee_saved() {
    g_fork_frame.r12 = kForkUserR12;
    g_fork_frame.r13 = kForkUserR13;
    g_fork_frame.r14 = kForkUserR14;
    g_fork_frame.r15 = kForkUserR15;
}

void retire_fork_child(cinux::proc::Task& child) {
    const auto kStackPhys = cinux::mm::DirectMapPhys(child.stack_base);
    cinux::mm::Pmm::self().free_pages(cinux::base::PhysAddr{kStackPhys}, cinux::proc::kStackOrder);
    delete &child;
}

void arm_fork_frame() {
    g_fork_frame          = cinux::syscall::SyscallFrame{};
    g_fork_frame.user_rsp = kForkUserRsp;
    g_fork_frame.rcx      = kForkUserRip;
    g_fork_frame.r11      = 0x202;
    g_fork_frame.rax      = 57;
}

void inspect_shared_cursor(const cinux::proc::Task& child, const cinux::proc::Task& parent,
                           unsigned long first_fd) {
    ASSERT_TRUE(child.files->get(first_fd).ok());
    ASSERT_EQ(parent.files->get(first_fd).value()->offset, 128UL);
    ASSERT_EQ(child.files->get(first_fd).value()->offset, 128UL);
    ASSERT_TRUE(child.files->get(first_fd).value() == parent.files->get(first_fd).value());
}

void inspect_fork_family(const cinux::proc::Task& child, const cinux::proc::Task& parent,
                         unsigned long first_fd) {
    ASSERT_TRUE(child.parent == &parent);
    ASSERT_TRUE(child.user_root != g_fork_world->root());
    ASSERT_EQ(child.user_root, child.space->root());
    ASSERT_TRUE(child.space.holds());
    ASSERT_TRUE(child.space.get() != g_fork_world.get());
    inspect_shared_cursor(child, parent, first_fd);
}

void inspect_fork_fpu(const cinux::proc::Task& child, const cinux::proc::Task& parent) {
    for (unsigned long index = 0; index < 64; ++index) {
        ASSERT_EQ(child.ctx.fpu.words[index], parent.entry_fpu.words[index]);
    }
    ASSERT_EQ(child.fpu_gate_depth, 0ULL);
}

void inspect_fork_child(const cinux::proc::Task& child, const cinux::proc::Task& parent,
                        unsigned long first_fd) {
    ASSERT_EQ(child.ctx.rip, reinterpret_cast<unsigned long long>(&ForkChildEntry));
    ASSERT_EQ(child.ctx.rsp + sizeof(cinux::syscall::SyscallFrame),
              child.stack_base + cinux::proc::kStackBytes);
    ASSERT_EQ(child.ctx.r12, kForkUserR12);
    ASSERT_EQ(child.ctx.r13, kForkUserR13);
    ASSERT_EQ(child.ctx.r14, kForkUserR14);
    ASSERT_EQ(child.ctx.r15, kForkUserR15);
    inspect_fork_fpu(child, parent);
    const auto* const kCopied =
        cinux::base::PtrAt<const cinux::syscall::SyscallFrame>(child.ctx.rsp);
    ASSERT_EQ(kCopied->user_rsp, kForkUserRsp);
    ASSERT_EQ(kCopied->rcx, kForkUserRip);
    inspect_fork_family(child, parent, first_fd);
}

void fork_exit_child() {
    cinux::proc::Scheduler::self().exit_current(42);
}

void fork_wait_parent_task() {
    auto&       scheduler = cinux::proc::Scheduler::self();
    auto* const kSelf     = scheduler.current();
    g_fork_world          = cinux::base::MakeShared<cinux::mm::AddressSpace>();
    cinux::base::safety::Check(g_fork_world.holds(), "fork wait world failed");
    kSelf->space = g_fork_world;
    cinux::base::safety::Check(g_fork_world->init(), "fork wait world failed");
    auto* const kChild = cinux::proc::TaskBuilder{}
                             .set_entry(fork_exit_child)
                             .set_name("fork_calf")
                             .set_parent(*kSelf)
                             .build();
    cinux::base::safety::Check(kChild != nullptr, "fork wait child failed");
    scheduler.seat(*kChild);
    ASSERT_TRUE(kChild->tgid == kChild->pid);
    g_receipt    = cinux::proc::ChildReceipt{};
    g_wait_ok    = scheduler.wait_child(-1, false, g_receipt);
    kSelf->space = {};
    g_fork_world = {};
}

void forking_parent_task() {
    const auto& scheduler = cinux::proc::Scheduler::self();
    auto* const kSelf     = scheduler.current();
    g_fork_world          = cinux::base::MakeShared<cinux::mm::AddressSpace>();
    g_fork_ranges         = cinux::base::MakeShared<cinux::mm::VmaStore>();
    cinux::base::safety::Check(g_fork_world.holds() && g_fork_ranges.holds(),
                               "fork fixture world failed");
    cinux::base::safety::Check(g_fork_world->init(), "fork fixture world failed");
    const cinux::base::PhysAddr kPage = cinux::mm::Pmm::self().allocate_page();
    cinux::base::safety::Check(kPage != cinux::base::PhysAddr{}, "fork fixture frame failed");
    cinux::base::safety::Check(g_fork_world->map_user(cinux::mm::kUserNullTop + 80_KiB, kPage.raw),
                               "fork fixture map failed");
    kSelf->space       = g_fork_world;
    kSelf->vmas        = g_fork_ranges;
    const auto kFirst  = kSelf->files->alloc(&cinux::test::FileRefs::kOps, &g_file_refs, {});
    const auto kSecond = kSelf->files->alloc(&cinux::test::FileRefs::kOps, &g_file_refs, {});
    cinux::base::safety::Check(kFirst.ok() && kSecond.ok(), "fork fixture fds failed");
    kSelf->files->get(kFirst.value()).value()->offset = 128;
    arm_fork_frame();
    stage_callee_saved();
    cinux::arch::SaveFpu(kSelf->entry_fpu);
    kSelf->entry_fpu.words[20] = kSentinelXmm0;
    cinux::proc::Task* const kChild =
        cinux::proc::BuildForkChild(reinterpret_cast<unsigned long long>(&g_fork_frame));
    g_fork_built = kChild != nullptr;
    if (kChild == nullptr) {
        return;
    }
    inspect_fork_child(*kChild, *kSelf, kFirst.value());
    retire_fork_child(*kChild);
    kSelf->space  = {};
    kSelf->vmas   = {};
    g_fork_world  = {};
    g_fork_ranges = {};
}

constexpr unsigned long long kCloneChildRsp = kForkUserRsp + 0x2000;
constexpr unsigned long long kCloneTls      = 0x0000000000700000ULL;

unsigned long long g_clone_flags = 0;
unsigned long long g_clone_tls   = 0;
bool               g_clone_built = false;

void inspect_clone_worlds(const cinux::proc::Task& child, const cinux::proc::Task& parent) {
    if ((g_clone_flags & cinux::syscall::kCloneVm) != 0) {
        ASSERT_TRUE(child.space.get() == parent.space.get());
        ASSERT_TRUE(child.vmas.get() == parent.vmas.get());
        ASSERT_EQ(child.user_root, parent.user_root);
        return;
    }
    ASSERT_TRUE(child.space.get() != parent.space.get());
    ASSERT_TRUE(child.vmas.get() != parent.vmas.get());
    ASSERT_EQ(child.user_root, child.space->root());
}

void inspect_clone_files(const cinux::proc::Task& child, const cinux::proc::Task& parent,
                         unsigned long only_fd) {
    if ((g_clone_flags & cinux::syscall::kCloneFiles) == 0) {
        ASSERT_TRUE(child.files.get() != parent.files.get());
        ASSERT_TRUE(child.files->get(only_fd).ok());
        return;
    }
    ASSERT_TRUE(child.files.get() == parent.files.get());
}

void inspect_clone_tls(const cinux::proc::Task& child) {
    if ((g_clone_flags & cinux::syscall::kCloneSetTls) == 0) {
        return;
    }
    ASSERT_EQ(child.ctx.fs_base, g_clone_tls);
}

void inspect_clone_child(const cinux::proc::Task& child, const cinux::proc::Task& parent,
                         unsigned long only_fd) {
    ASSERT_EQ(child.ctx.rip, reinterpret_cast<unsigned long long>(&ForkChildEntry));
    const auto* const kCopied =
        cinux::base::PtrAt<const cinux::syscall::SyscallFrame>(child.ctx.rsp);
    ASSERT_EQ(kCopied->user_rsp, kCloneChildRsp);
    ASSERT_EQ(kCopied->rcx, kForkUserRip);
    inspect_fork_fpu(child, parent);
    inspect_clone_files(child, parent, only_fd);
    inspect_clone_tls(child);
    inspect_clone_worlds(child, parent);
}

void cloning_parent_task() {
    const auto& scheduler = cinux::proc::Scheduler::self();
    auto* const kSelf     = scheduler.current();
    g_fork_world          = cinux::base::MakeShared<cinux::mm::AddressSpace>();
    g_fork_ranges         = cinux::base::MakeShared<cinux::mm::VmaStore>();
    cinux::base::safety::Check(g_fork_world.holds() && g_fork_ranges.holds(),
                               "clone fixture world failed");
    cinux::base::safety::Check(g_fork_world->init(), "clone fixture world failed");
    kSelf->space     = g_fork_world;
    kSelf->vmas      = g_fork_ranges;
    const auto kOnly = kSelf->files->alloc(&cinux::test::FileRefs::kOps, &g_file_refs, {});
    cinux::base::safety::Check(kOnly.ok(), "clone fixture fd failed");
    kSelf->files->get(kOnly.value()).value()->offset = 64;
    arm_fork_frame();
    stage_callee_saved();
    cinux::arch::SaveFpu(kSelf->entry_fpu);
    kSelf->entry_fpu.words[20] = kSentinelXmm0;
    const cinux::proc::ClonePlan kPlan{
        .flags      = g_clone_flags,
        .user_stack = kCloneChildRsp,
        .tls        = g_clone_tls,
    };
    cinux::proc::Task* const kChild =
        cinux::proc::BuildCloneChild(reinterpret_cast<unsigned long long>(&g_fork_frame), kPlan);
    g_clone_built = kChild != nullptr;
    if (kChild == nullptr) {
        return;
    }
    inspect_clone_child(*kChild, *kSelf, kOnly.value());
    retire_fork_child(*kChild);
    kSelf->space  = {};
    kSelf->vmas   = {};
    g_fork_world  = {};
    g_fork_ranges = {};
}

}  // namespace

TEST("fork: a seated child exits into the parent's wait") {
    auto&               scheduler = cinux::proc::Scheduler::self();
    const auto&         ledger    = cinux::mm::Pmm::self();
    const unsigned long kBefore   = ledger.free_page_count();
    g_wait_ok                     = false;
    seat_built(cinux::proc::TaskBuilder{}
                   .set_entry(fork_wait_parent_task)
                   .set_name("fork_waiter")
                   .build());
    scheduler.run_until_done();
    ASSERT_TRUE(g_wait_ok);
    ASSERT_TRUE(g_receipt.code == 42);
    ASSERT_TRUE(g_receipt.pid != 0);
    ASSERT_EQ(ledger.free_page_count(), kBefore);
}

TEST("fork: a built child carries the frame, the files, and the family") {
    auto&               scheduler = cinux::proc::Scheduler::self();
    const auto&         ledger    = cinux::mm::Pmm::self();
    const unsigned long kBefore   = ledger.free_page_count();
    g_fork_built                  = false;
    seat_built(
        cinux::proc::TaskBuilder{}.set_entry(forking_parent_task).set_name("fparent").build());
    scheduler.run_until_done();
    ASSERT_TRUE(g_fork_built);
    ASSERT_EQ(ledger.free_page_count(), kBefore);
}

TEST("clone: the shared limbs are the parent's own, the frame moves house") {
    auto&               scheduler = cinux::proc::Scheduler::self();
    const auto&         ledger    = cinux::mm::Pmm::self();
    const unsigned long kBefore   = ledger.free_page_count();
    g_clone_built                 = false;
    g_clone_flags =
        cinux::syscall::kCloneVm | cinux::syscall::kCloneFiles | cinux::syscall::kCloneSetTls;
    g_clone_tls = kCloneTls;
    seat_built(
        cinux::proc::TaskBuilder{}.set_entry(cloning_parent_task).set_name("cparent").build());
    scheduler.run_until_done();
    ASSERT_TRUE(g_clone_built);
    ASSERT_EQ(ledger.free_page_count(), kBefore);
}

TEST("clone: without the flags it is fork with a moved stack") {
    auto&               scheduler = cinux::proc::Scheduler::self();
    const auto&         ledger    = cinux::mm::Pmm::self();
    const unsigned long kBefore   = ledger.free_page_count();
    g_clone_built                 = false;
    g_clone_flags                 = 0;
    g_clone_tls                   = 0;
    seat_built(
        cinux::proc::TaskBuilder{}.set_entry(cloning_parent_task).set_name("cparent").build());
    scheduler.run_until_done();
    ASSERT_TRUE(g_clone_built);
    ASSERT_EQ(ledger.free_page_count(), kBefore);
}

TEST("wait: four generations preserve every parked ancestor") {
    auto&               scheduler = cinux::proc::Scheduler::self();
    const auto&         ledger    = cinux::mm::Pmm::self();
    const unsigned long kBefore   = ledger.free_page_count();
    g_nested_reaped               = 0;
    seat_built(
        cinux::proc::TaskBuilder{}.set_entry(nested_init_task).set_name("nested_init").build());
    scheduler.run_until_done();
    ASSERT_EQ(g_nested_reaped, 3U);
    ASSERT_EQ(scheduler.offspring_count(), 0ULL);
    ASSERT_EQ(ledger.free_page_count(), kBefore);
}

namespace {

bool               g_halt_done   = false;
unsigned int       g_halt_visits = 0;
cinux::proc::Task* g_halted_task = nullptr;

void self_stop_task() {
    cinux::proc::Scheduler::self().stop_current();
    g_halt_done = true;
    cinux::proc::Scheduler::self().exit_current(3);
}

void halted_wake_task() {
    cinux::proc::Scheduler::self().stop_current();
    g_halt_visits++;
    cinux::proc::Scheduler::self().exit_current(7);
}

void waker_task() {
    cinux::proc::Scheduler::self().resume(*g_halted_task);
    cinux::proc::Scheduler::self().exit_current();
}

unsigned int             g_futex_word_a   = 1;
unsigned int             g_futex_word_b   = 1;
bool                     g_futex_a_awake  = false;
bool                     g_futex_b_awake  = false;
long long                g_futex_wake_a   = 0;
long long                g_futex_wake_b   = 0;
cinux::base::KernelError g_futex_mismatch = cinux::base::KernelError::kOk;

void futex_mismatch_task() {
    g_futex_mismatch = cinux::proc::futex::WaitOnUserWord(
        reinterpret_cast<unsigned long long>(&g_futex_word_a), 999);
    cinux::proc::Scheduler::self().exit_current();
}

void futex_waiter_a_task() {
    cinux::base::safety::Check(
        cinux::proc::futex::WaitOnUserWord(reinterpret_cast<unsigned long long>(&g_futex_word_a),
                                           1) == cinux::base::KernelError::kOk,
        "waiter a did not return ok");
    g_futex_a_awake = true;
    cinux::proc::Scheduler::self().exit_current();
}

void futex_waiter_b_task() {
    cinux::base::safety::Check(
        cinux::proc::futex::WaitOnUserWord(reinterpret_cast<unsigned long long>(&g_futex_word_b),
                                           1) == cinux::base::KernelError::kOk,
        "waiter b did not return ok");
    g_futex_b_awake = true;
    cinux::proc::Scheduler::self().exit_current();
}

void futex_waker_task() {
    while (!g_futex_a_awake) {
        g_futex_wake_a += cinux::proc::futex::WakeUserWord(
            reinterpret_cast<unsigned long long>(&g_futex_word_a), 8);
        cinux::proc::Scheduler::self().yield();
    }
    while (!g_futex_b_awake) {
        g_futex_wake_b += cinux::proc::futex::WakeUserWord(
            reinterpret_cast<unsigned long long>(&g_futex_word_b), 8);
        cinux::proc::Scheduler::self().yield();
    }
    cinux::proc::Scheduler::self().exit_current();
}

}  // namespace

TEST("stop: a task that halts itself leaves the queue with its stack held") {
    auto&               scheduler = cinux::proc::Scheduler::self();
    const auto&         ledger    = cinux::mm::Pmm::self();
    const unsigned long kBefore   = ledger.free_page_count();
    g_halt_done                   = false;
    cinux::proc::Task* const kTask =
        cinux::proc::TaskBuilder{}.set_entry(self_stop_task).set_name("halted").build();
    seat_built(kTask);
    scheduler.run_until_done();
    ASSERT_TRUE(!g_halt_done);
    ASSERT_TRUE(kTask->state == cinux::proc::TaskState::kStopped);
    scheduler.resume(*kTask);
    scheduler.run_until_done();
    ASSERT_TRUE(g_halt_done);
    ASSERT_EQ(scheduler.offspring_count(), 0ULL);
    ASSERT_EQ(ledger.free_page_count(), kBefore);
}

TEST("stop: a sibling's resume lifts the halt mid-schedule") {
    auto&               scheduler = cinux::proc::Scheduler::self();
    const auto&         ledger    = cinux::mm::Pmm::self();
    const unsigned long kBefore   = ledger.free_page_count();
    g_halt_visits                 = 0;
    g_halted_task =
        cinux::proc::TaskBuilder{}.set_entry(halted_wake_task).set_name("halted").build();
    seat_built(g_halted_task);
    seat_built(cinux::proc::TaskBuilder{}.set_entry(waker_task).set_name("waker").build());
    scheduler.run_until_done();
    ASSERT_EQ(g_halt_visits, 1U);
    ASSERT_EQ(scheduler.offspring_count(), 0ULL);
    ASSERT_EQ(ledger.free_page_count(), kBefore);
}

TEST("sched: a futex wait on a moved word refuses to park") {
    auto& scheduler  = cinux::proc::Scheduler::self();
    g_futex_word_a   = 1;
    g_futex_mismatch = cinux::base::KernelError::kOk;
    seat_built(
        cinux::proc::TaskBuilder{}.set_entry(futex_mismatch_task).set_name("f-miss").build());
    scheduler.run_until_done();
    ASSERT_TRUE(g_futex_mismatch == cinux::base::KernelError::kWouldBlock);
}

TEST("sched: waking an untouched futex word wakes nobody") {
    ASSERT_TRUE(cinux::proc::futex::WakeUserWord(
                    reinterpret_cast<unsigned long long>(&g_futex_word_a), 8) == 0);
}

TEST("sched: futex waiters wake only on their own word") {
    auto& scheduler = cinux::proc::Scheduler::self();
    g_futex_word_a  = 1;
    g_futex_word_b  = 1;
    g_futex_a_awake = false;
    g_futex_b_awake = false;
    g_futex_wake_a  = 0;
    g_futex_wake_b  = 0;
    seat_built(
        cinux::proc::TaskBuilder{}.set_entry(futex_waiter_a_task).set_name("f-wait-a").build());
    seat_built(
        cinux::proc::TaskBuilder{}.set_entry(futex_waiter_b_task).set_name("f-wait-b").build());
    seat_built(cinux::proc::TaskBuilder{}.set_entry(futex_waker_task).set_name("f-waker").build());
    scheduler.set_preemption(true);
    scheduler.run_until_done();
    scheduler.set_preemption(false);
    ASSERT_TRUE(g_futex_wake_a == 1);
    ASSERT_TRUE(g_futex_wake_b == 1);
    ASSERT_TRUE(g_futex_a_awake);
    ASSERT_TRUE(g_futex_b_awake);
    ASSERT_EQ(scheduler.offspring_count(), 0ULL);
}

namespace {

unsigned long long g_thr_process_pid    = 0;
bool               g_thr_thread_reaped  = false;
bool               g_thr_process_reaped = false;
unsigned long long g_thr_receipt_pid    = 0;
long long          g_thr_receipt_code   = 0;

void silent_exit_task() {
    cinux::proc::Scheduler::self().exit_current(9);
}

void thread_join_probe_task() {
    auto&       scheduler = cinux::proc::Scheduler::self();
    auto* const kThread   = cinux::proc::TaskBuilder{}
                                .set_entry(silent_exit_task)
                                .set_name("thread_member")
                                .set_parent(*scheduler.current())
                                .build();
    cinux::base::safety::Check(kThread != nullptr, "thread member failed to build");
    scheduler.seat(*kThread);
    kThread->tgid = scheduler.current()->pid;

    auto* const kProcess = cinux::proc::TaskBuilder{}
                               .set_entry(silent_exit_task)
                               .set_name("proper_child")
                               .set_parent(*scheduler.current())
                               .build();
    cinux::base::safety::Check(kProcess != nullptr, "proper child failed to build");
    kProcess->clear_child_tid = 0x0080000000000000ULL;
    scheduler.seat(*kProcess);

    cinux::proc::ChildReceipt receipt{};
    g_thr_process_pid = kProcess->pid;
    g_thr_process_reaped =
        scheduler.wait_child(static_cast<long long>(kProcess->pid), false, receipt);
    g_thr_receipt_pid  = receipt.pid;
    g_thr_receipt_code = receipt.code;
    g_thr_thread_reaped =
        scheduler.wait_child(static_cast<long long>(kThread->pid), false, receipt);
    scheduler.exit_current();
}

}  // namespace

TEST("thread: a group member exits to reclaim and answers no wait") {
    auto& scheduler      = cinux::proc::Scheduler::self();
    g_thr_process_pid    = 0;
    g_thr_thread_reaped  = false;
    g_thr_process_reaped = false;
    g_thr_receipt_pid    = 0;
    g_thr_receipt_code   = 0;
    seat_built(cinux::proc::TaskBuilder{}
                   .set_entry(thread_join_probe_task)
                   .set_name("join_probe")
                   .build());
    scheduler.run_until_done();
    ASSERT_TRUE(g_thr_process_reaped);
    ASSERT_EQ(g_thr_receipt_pid, g_thr_process_pid);
    ASSERT_EQ(g_thr_receipt_code, 9LL);
    ASSERT_TRUE(!g_thr_thread_reaped);
    ASSERT_EQ(scheduler.offspring_count(), 0ULL);
}
