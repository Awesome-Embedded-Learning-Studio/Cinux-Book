/**
 * @file kernel/test/test_scheduler.cpp
 * @brief QEMU in-kernel integration tests for process/scheduler (019)
 *
 * Runs inside QEMU as part of the big kernel test suite.  Exercises the
 * real Scheduler, TaskBuilder, RoundRobin, and context_switch with actual
 * PMM/VMM/Heap backing.  Tests:
 *   - TaskBuilder: build with entry/name/priority
 *   - RoundRobin: enqueue/dequeue/pick_next with real Task objects
 *   - Scheduler: init, add_task, register_class
 *   - context_switch: cooperative switch between two kernel tasks
 *   - Task lifecycle: state transitions through Ready/Running
 *
 * Preconditions (set up by main_test.cpp before this runs):
 *   - Serial port initialised (kprintf works)
 *   - GDT and IDT loaded
 *   - PMM initialised
 *   - VMM initialised
 *   - Heap initialised
 *   - AddressSpace kernel PML4 initialised
 */

#include <stddef.h>
#include <stdint.h>

namespace {
void kmemset(void* dst, int val, size_t n) {
    auto* p = static_cast<uint8_t*>(dst);
    for (size_t i = 0; i < n; i++)
        p[i] = static_cast<uint8_t>(val);
}
}  // anonymous namespace

#include "big_kernel_test.h"
#include "kernel/proc/process.hpp"
#include "kernel/proc/scheduler.hpp"

using cinux::proc::Task;
using cinux::proc::TaskBuilder;
using cinux::proc::TaskState;
using cinux::proc::CpuContext;
using cinux::proc::Scheduler;
using cinux::proc::RoundRobin;
using cinux::proc::SchedulingClass;
using cinux::proc::context_switch;

// ============================================================
// Test 1: TaskBuilder creates a valid kernel task
// ============================================================

namespace test_task_builder {

void dummy_entry() {
    // Never actually runs in this test; just a non-null entry point
}

void test_build_basic_task() {
    Task* task =
        TaskBuilder().set_entry(dummy_entry).set_name("test_basic").set_priority(1).build();

    TEST_ASSERT_NOT_NULL(task);
    TEST_ASSERT_EQ(task->tid, 1u);
    TEST_ASSERT_EQ(static_cast<int>(task->state), static_cast<int>(TaskState::Ready));
    TEST_ASSERT(task->name != nullptr);
    // Name should be "test_basic"
    TEST_ASSERT_EQ(task->priority, 1u);
    TEST_ASSERT_NULL(task->addr_space);
    TEST_ASSERT_NULL(task->sched_class);

    // CpuContext should have rip pointing to dummy_entry
    TEST_ASSERT_EQ(task->ctx.rip, reinterpret_cast<uint64_t>(dummy_entry));

    // Stack should be non-zero and aligned
    TEST_ASSERT_NE(task->kernel_stack, 0u);
    TEST_ASSERT_NE(task->kernel_stack_top, 0u);
    TEST_ASSERT_GT(task->kernel_stack_top, task->kernel_stack);

    // Stack overflow magic at the bottom
    TEST_ASSERT_EQ(*reinterpret_cast<uint64_t*>(task->kernel_stack), TaskBuilder::STACK_MAGIC);
}

}  // namespace test_task_builder

// ============================================================
// Test 2: TaskBuilder returns nullptr for null entry
// ============================================================

namespace test_task_builder_null {

void test_build_null_entry() {
    Task* task = TaskBuilder().set_name("null_entry").build();

    TEST_ASSERT_NULL(task);
}

}  // namespace test_task_builder_null

// ============================================================
// Test 3: Scheduler init registers default RoundRobin
// ============================================================

namespace test_scheduler_init {

void test_init_registers_default() {
    Scheduler::init();

    // After init, current should be null
    TEST_ASSERT_NULL(Scheduler::current());

    // Adding a task with null sched_class should use default RR
    Task* task =
        TaskBuilder().set_entry(test_task_builder::dummy_entry).set_name("init_test").build();
    TEST_ASSERT_NOT_NULL(task);

    Scheduler::add_task(task);

    // sched_class should have been assigned
    TEST_ASSERT_NOT_NULL(task->sched_class);
    TEST_ASSERT_EQ(static_cast<int>(task->state), static_cast<int>(TaskState::Ready));
}

}  // namespace test_scheduler_init

// ============================================================
// Test 4: RoundRobin enqueue/dequeue/pick_next with real tasks
// ============================================================

namespace test_round_robin {

void test_enqueue_dequeue() {
    RoundRobin rr;

    Task* t1 = TaskBuilder().set_entry(test_task_builder::dummy_entry).set_name("rr_1").build();
    Task* t2 = TaskBuilder().set_entry(test_task_builder::dummy_entry).set_name("rr_2").build();
    Task* t3 = TaskBuilder().set_entry(test_task_builder::dummy_entry).set_name("rr_3").build();

    TEST_ASSERT_NOT_NULL(t1);
    TEST_ASSERT_NOT_NULL(t2);
    TEST_ASSERT_NOT_NULL(t3);

    rr.enqueue(t1);
    rr.enqueue(t2);
    rr.enqueue(t3);

    // pick_next should rotate through t1, t2, t3
    Task* n1 = rr.pick_next();
    TEST_ASSERT_EQ(n1, t1);
    TEST_ASSERT_EQ(static_cast<int>(n1->state), static_cast<int>(TaskState::Running));

    Task* n2 = rr.pick_next();
    TEST_ASSERT_EQ(n2, t2);

    Task* n3 = rr.pick_next();
    TEST_ASSERT_EQ(n3, t3);

    // Wrap around
    Task* n4 = rr.pick_next();
    TEST_ASSERT_EQ(n4, t1);
}

void test_dequeue_middle() {
    RoundRobin rr;

    Task* t1 = TaskBuilder().set_entry(test_task_builder::dummy_entry).set_name("dq_1").build();
    Task* t2 = TaskBuilder().set_entry(test_task_builder::dummy_entry).set_name("dq_2").build();
    Task* t3 = TaskBuilder().set_entry(test_task_builder::dummy_entry).set_name("dq_3").build();

    rr.enqueue(t1);
    rr.enqueue(t2);
    rr.enqueue(t3);

    rr.dequeue(t2);

    // After dequeuing t2, pick_next should give t1 then t3
    TEST_ASSERT_EQ(rr.pick_next(), t1);
    TEST_ASSERT_EQ(rr.pick_next(), t3);
}

void test_empty_pick_next() {
    RoundRobin rr;
    TEST_ASSERT_NULL(rr.pick_next());
}

}  // namespace test_round_robin

// ============================================================
// Test 5: CpuContext struct layout (kernel-side verification)
// ============================================================

namespace test_cpu_context {

void test_layout() {
    TEST_ASSERT_EQ(sizeof(CpuContext), 96u);
    TEST_ASSERT_EQ(offsetof(CpuContext, r15), 0u);
    TEST_ASSERT_EQ(offsetof(CpuContext, r14), 8u);
    TEST_ASSERT_EQ(offsetof(CpuContext, r13), 16u);
    TEST_ASSERT_EQ(offsetof(CpuContext, r12), 24u);
    TEST_ASSERT_EQ(offsetof(CpuContext, rbp), 32u);
    TEST_ASSERT_EQ(offsetof(CpuContext, rbx), 40u);
    TEST_ASSERT_EQ(offsetof(CpuContext, rsp), 48u);
    TEST_ASSERT_EQ(offsetof(CpuContext, rip), 56u);
    TEST_ASSERT_EQ(offsetof(CpuContext, gs_base), 64u);
    TEST_ASSERT_EQ(offsetof(CpuContext, kgs_base), 72u);
    TEST_ASSERT_EQ(offsetof(CpuContext, fs_base), 80u);
    TEST_ASSERT_EQ(alignof(CpuContext), 16u);
}

}  // namespace test_cpu_context

// ============================================================
// Test 6: context_switch cooperative multitasking
// ============================================================

namespace test_context_switch {

// Volatile counters to prevent compiler optimization
static volatile int  task_a_count = 0;
static volatile int  task_b_count = 0;
static volatile bool done         = false;

// We need a boot context and two task contexts
static CpuContext boot_ctx;
static CpuContext task_a_ctx;
static CpuContext task_b_ctx;

static void task_a_func() {
    task_a_count++;
    // Switch to task B
    context_switch(&task_a_ctx, &task_b_ctx);
    // Comes back here after B yields
    task_a_count++;
    // Done
    done = true;
    // Switch back to boot
    context_switch(&task_a_ctx, &boot_ctx);
}

static void task_b_func() {
    task_b_count++;
    // Switch back to task A
    context_switch(&task_b_ctx, &task_a_ctx);
    // Should not reach here in this test
}

void test_cooperative_switch() {
    task_a_count = 0;
    task_b_count = 0;
    done         = false;

    // Zero out contexts
    kmemset(&boot_ctx, 0, sizeof(boot_ctx));
    kmemset(&task_a_ctx, 0, sizeof(task_a_ctx));
    kmemset(&task_b_ctx, 0, sizeof(task_b_ctx));

    // Set up task A context: entry = task_a_func
    // Use a small static stack buffer for each task
    static uint8_t stack_a[4096] __attribute__((aligned(16)));
    static uint8_t stack_b[4096] __attribute__((aligned(16)));

    task_a_ctx.rip = reinterpret_cast<uint64_t>(task_a_func);
    task_a_ctx.rsp = reinterpret_cast<uint64_t>(&stack_a[4096]);

    task_b_ctx.rip = reinterpret_cast<uint64_t>(task_b_func);
    task_b_ctx.rsp = reinterpret_cast<uint64_t>(&stack_b[4096]);

    // Switch from boot to task A
    context_switch(&boot_ctx, &task_a_ctx);

    // We come back here when task A switches back to boot
    TEST_ASSERT_TRUE(done);
    TEST_ASSERT_EQ(task_a_count, 2);
    TEST_ASSERT_EQ(task_b_count, 1);
}

}  // namespace test_context_switch

// ============================================================
// Test 7: Task state transitions
// ============================================================

namespace test_task_state {

void test_state_transitions() {
    Task* task =
        TaskBuilder().set_entry(test_task_builder::dummy_entry).set_name("state_test").build();
    TEST_ASSERT_NOT_NULL(task);

    // After build, state should be Ready
    TEST_ASSERT_EQ(static_cast<int>(task->state), static_cast<int>(TaskState::Ready));

    // Manually set to Running
    task->state = TaskState::Running;
    TEST_ASSERT_EQ(static_cast<int>(task->state), static_cast<int>(TaskState::Running));

    // Manually set to Blocked
    task->state = TaskState::Blocked;
    TEST_ASSERT_EQ(static_cast<int>(task->state), static_cast<int>(TaskState::Blocked));

    // Manually set to Dead
    task->state = TaskState::Dead;
    TEST_ASSERT_EQ(static_cast<int>(task->state), static_cast<int>(TaskState::Dead));
}

}  // namespace test_task_state

// ============================================================
// Test 8: TaskBuilder default values
// ============================================================

namespace test_task_builder_defaults {

void test_default_name() {
    Task* task = TaskBuilder().set_entry(test_task_builder::dummy_entry).build();
    TEST_ASSERT_NOT_NULL(task);
    // Default name should be "unnamed"
    TEST_ASSERT_NOT_NULL(task->name);
}

void test_default_priority() {
    Task* task = TaskBuilder().set_entry(test_task_builder::dummy_entry).build();
    TEST_ASSERT_NOT_NULL(task);
    TEST_ASSERT_EQ(task->priority, 0u);
}

void test_increasing_tid() {
    Task* t1 = TaskBuilder().set_entry(test_task_builder::dummy_entry).set_name("tid_a").build();
    Task* t2 = TaskBuilder().set_entry(test_task_builder::dummy_entry).set_name("tid_b").build();

    TEST_ASSERT_NOT_NULL(t1);
    TEST_ASSERT_NOT_NULL(t2);
    TEST_ASSERT_GT(t2->tid, t1->tid);
}

}  // namespace test_task_builder_defaults

// ============================================================
// Test 9: Scheduler new APIs (020)
// ============================================================

namespace test_scheduler_new {

void test_is_initialized() {
    Scheduler::init();
    TEST_ASSERT_TRUE(Scheduler::is_initialized());
}

void test_remove_task() {
    Scheduler::init();

    Task* task =
        TaskBuilder().set_entry(test_task_builder::dummy_entry).set_name("remove_test").build();
    TEST_ASSERT_NOT_NULL(task);

    Scheduler::add_task(task);
    TEST_ASSERT_NOT_NULL(task->sched_class);

    Scheduler::remove_task(task);
    TEST_ASSERT_EQ(static_cast<int>(task->state), static_cast<int>(TaskState::Dead));
}

void test_block_unblock() {
    Scheduler::init();

    Task* task =
        TaskBuilder().set_entry(test_task_builder::dummy_entry).set_name("block_test").build();
    TEST_ASSERT_NOT_NULL(task);

    Scheduler::add_task(task);
    TEST_ASSERT_EQ(static_cast<int>(task->state), static_cast<int>(TaskState::Ready));

    Scheduler::block(task, "test block");
    TEST_ASSERT_EQ(static_cast<int>(task->state), static_cast<int>(TaskState::Blocked));

    Scheduler::unblock(task);
    TEST_ASSERT_EQ(static_cast<int>(task->state), static_cast<int>(TaskState::Ready));
}

}  // namespace test_scheduler_new

// ============================================================
// Test 10: RoundRobin spinlock correctness (028d)
// ============================================================

namespace test_round_robin_locked {

void test_locked_enqueue_dequeue() {
    RoundRobin rr;

    Task* t1 = TaskBuilder().set_entry(test_task_builder::dummy_entry).set_name("lk_1").build();
    Task* t2 = TaskBuilder().set_entry(test_task_builder::dummy_entry).set_name("lk_2").build();
    Task* t3 = TaskBuilder().set_entry(test_task_builder::dummy_entry).set_name("lk_3").build();

    TEST_ASSERT_NOT_NULL(t1);
    TEST_ASSERT_NOT_NULL(t2);
    TEST_ASSERT_NOT_NULL(t3);

    rr.enqueue(t1);
    rr.enqueue(t2);
    rr.enqueue(t3);

    Task* n1 = rr.pick_next();
    TEST_ASSERT_EQ(n1, t1);
    TEST_ASSERT_EQ(static_cast<int>(n1->state), static_cast<int>(TaskState::Running));

    Task* n2 = rr.pick_next();
    TEST_ASSERT_EQ(n2, t2);

    Task* n3 = rr.pick_next();
    TEST_ASSERT_EQ(n3, t3);

    Task* n4 = rr.pick_next();
    TEST_ASSERT_EQ(n4, t1);
}

void test_locked_dequeue_middle() {
    RoundRobin rr;

    Task* t1 = TaskBuilder().set_entry(test_task_builder::dummy_entry).set_name("ldq_1").build();
    Task* t2 = TaskBuilder().set_entry(test_task_builder::dummy_entry).set_name("ldq_2").build();
    Task* t3 = TaskBuilder().set_entry(test_task_builder::dummy_entry).set_name("ldq_3").build();

    rr.enqueue(t1);
    rr.enqueue(t2);
    rr.enqueue(t3);

    rr.dequeue(t2);

    TEST_ASSERT_EQ(rr.pick_next(), t1);
    TEST_ASSERT_EQ(rr.pick_next(), t3);
}

void test_locked_fifo_order() {
    RoundRobin rr;

    Task* tasks[8];
    for (int i = 0; i < 8; i++) {
        char name[4];
        name[0]  = 'f';
        name[1]  = static_cast<char>('0' + i);
        name[2]  = '\0';
        tasks[i] = TaskBuilder().set_entry(test_task_builder::dummy_entry).set_name(name).build();
        TEST_ASSERT_NOT_NULL(tasks[i]);
        rr.enqueue(tasks[i]);
    }

    for (int i = 0; i < 8; i++) {
        Task* n = rr.pick_next();
        TEST_ASSERT_EQ(n, tasks[i]);
    }
}

}  // namespace test_round_robin_locked

// ============================================================
// Test 11: SchedulingClass policy hooks (F3-M4 batch 1)
// ============================================================

namespace test_sched_class_hooks {

void test_task_tick_quantum() {
    RoundRobin rr;
    Task* t = TaskBuilder().set_entry(test_task_builder::dummy_entry).set_name("tick_q").build();
    TEST_ASSERT_NOT_NULL(t);

    rr.enqueue(t);
    rr.pick_next();  // select t, recharge its quantum to DEFAULT_TIME_SLICE

    // The first (DEFAULT_TIME_SLICE - 1) ticks do not request preemption...
    for (int i = 1; i < Scheduler::DEFAULT_TIME_SLICE; i++) {
        TEST_ASSERT_FALSE(rr.task_tick(t));
    }
    // ...the DEFAULT_TIME_SLICE-th tick exhausts the quantum and recharges.
    TEST_ASSERT_TRUE(rr.task_tick(t));
    // After recharging the task again has a full slice.
    TEST_ASSERT_FALSE(rr.task_tick(t));
}

void test_task_fork_inherits_priority() {
    RoundRobin rr;
    Task*      parent = TaskBuilder()
                            .set_entry(test_task_builder::dummy_entry)
                            .set_name("fork_p")
                            .set_priority(7)
                            .build();
    Task*      child  = TaskBuilder()
                            .set_entry(test_task_builder::dummy_entry)
                            .set_name("fork_c")
                            .set_priority(0)
                            .build();
    TEST_ASSERT_NOT_NULL(parent);
    TEST_ASSERT_NOT_NULL(child);

    rr.task_fork(parent, child);
    TEST_ASSERT_EQ(child->priority, 7u);
}

void test_task_deadline_default_zero() {
    // RoundRobin does not override task_deadline, so the base default (0,
    // "not deadline-based") applies.
    RoundRobin rr;
    Task*      t = TaskBuilder().set_entry(test_task_builder::dummy_entry).set_name("dl").build();
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_EQ(rr.task_deadline(t), 0u);
}

}  // namespace test_sched_class_hooks

// ============================================================
// Test 12: Priority-aware RoundRobin (F3-M4 batch 2)
// ============================================================

namespace test_priority {

void test_priority_picks_lowest_value() {
    RoundRobin rr;
    Task*      hi = TaskBuilder()
                        .set_entry(test_task_builder::dummy_entry)
                        .set_name("hi")
                        .set_priority(0)
                        .build();
    Task*      lo = TaskBuilder()
                        .set_entry(test_task_builder::dummy_entry)
                        .set_name("lo")
                        .set_priority(10)
                        .build();
    TEST_ASSERT_NOT_NULL(hi);
    TEST_ASSERT_NOT_NULL(lo);

    rr.enqueue(hi);  // priority 0
    rr.enqueue(lo);  // priority 10

    // Lower value = higher priority: hi is always selected while ready.
    TEST_ASSERT_EQ(rr.pick_next(), hi);
    // hi is re-enqueued and is still the lowest value, so it is picked again
    // (strict priority -- lo is starved while a higher-priority task is ready).
    TEST_ASSERT_EQ(rr.pick_next(), hi);
}

void test_priority_round_robin_within_level() {
    RoundRobin rr;
    Task*      a = TaskBuilder()
                       .set_entry(test_task_builder::dummy_entry)
                       .set_name("a")
                       .set_priority(5)
                       .build();
    Task*      b = TaskBuilder()
                       .set_entry(test_task_builder::dummy_entry)
                       .set_name("b")
                       .set_priority(5)
                       .build();
    Task*      c = TaskBuilder()
                       .set_entry(test_task_builder::dummy_entry)
                       .set_name("c")
                       .set_priority(9)
                       .build();
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_NOT_NULL(c);

    rr.enqueue(a);
    rr.enqueue(b);
    rr.enqueue(c);

    // Equal-priority tasks (a, b) round-robin; c (lower priority) is starved.
    TEST_ASSERT_EQ(rr.pick_next(), a);
    TEST_ASSERT_EQ(rr.pick_next(), b);
    TEST_ASSERT_EQ(rr.pick_next(), a);
    TEST_ASSERT_EQ(rr.pick_next(), b);
}

void test_priority_ignores_enqueue_order() {
    RoundRobin rr;
    Task*      first  = TaskBuilder()
                            .set_entry(test_task_builder::dummy_entry)
                            .set_name("first")
                            .set_priority(20)
                            .build();
    Task*      second = TaskBuilder()
                            .set_entry(test_task_builder::dummy_entry)
                            .set_name("second")
                            .set_priority(1)
                            .build();
    rr.enqueue(first);   // enqueued first but lower priority
    rr.enqueue(second);  // enqueued later but higher priority

    // pick_next selects by priority, not enqueue order.
    TEST_ASSERT_EQ(rr.pick_next(), second);
}

}  // namespace test_priority

// ============================================================
// Test 13: Multi-class consultation (F3-M4 batch 3)
// ============================================================

namespace test_multi_class {

// A class that is always empty.
class EmptyClass : public SchedulingClass {
public:
    void        enqueue(Task*) override {}
    void        dequeue(Task*) override {}
    Task*       pick_next() override { return nullptr; }
    const char* name() const override { return "EmptyClass"; }
};

// A class that holds a single task and drains it on pick_next.
class OneShotClass : public SchedulingClass {
public:
    void  enqueue(Task* t) override { task_ = t; }
    void  dequeue(Task*) override { task_ = nullptr; }
    Task* pick_next() override {
        Task* t = task_;
        task_   = nullptr;
        return t;
    }
    const char* name() const override { return "OneShotClass"; }

private:
    Task* task_ = nullptr;
};

void test_pick_next_from_skips_empty() {
    EmptyClass   a;
    OneShotClass b;
    EmptyClass   c;
    Task* mine = TaskBuilder().set_entry(test_task_builder::dummy_entry).set_name("mc").build();
    b.enqueue(mine);

    SchedulingClass* classes[3] = {&a, &b, &c};
    // a empty -> skip; b returns mine -> selected; c never asked.
    TEST_ASSERT_EQ(Scheduler::pick_next_from(classes, 3), mine);
}

void test_pick_next_from_all_empty() {
    EmptyClass       a;
    EmptyClass       b;
    SchedulingClass* classes[2] = {&a, &b};
    TEST_ASSERT_NULL(Scheduler::pick_next_from(classes, 2));
}

void test_pick_next_from_first_class_wins() {
    OneShotClass a;
    OneShotClass b;
    Task* first = TaskBuilder().set_entry(test_task_builder::dummy_entry).set_name("first").build();
    Task* second =
        TaskBuilder().set_entry(test_task_builder::dummy_entry).set_name("second").build();
    a.enqueue(first);
    b.enqueue(second);

    SchedulingClass* classes[2] = {&a, &b};
    // a (index 0) wins; b is never consulted.
    TEST_ASSERT_EQ(Scheduler::pick_next_from(classes, 2), first);
}

}  // namespace test_multi_class

// ============================================================
// Entry point
// ============================================================

extern "C" void run_scheduler_tests() {
    TEST_SECTION("Scheduler/Process Tests (020)");

    RUN_TEST(test_task_builder::test_build_basic_task);
    RUN_TEST(test_task_builder_null::test_build_null_entry);
    RUN_TEST(test_scheduler_init::test_init_registers_default);
    RUN_TEST(test_round_robin::test_enqueue_dequeue);
    RUN_TEST(test_round_robin::test_dequeue_middle);
    RUN_TEST(test_round_robin::test_empty_pick_next);
    RUN_TEST(test_cpu_context::test_layout);
    RUN_TEST(test_context_switch::test_cooperative_switch);
    RUN_TEST(test_task_state::test_state_transitions);
    RUN_TEST(test_task_builder_defaults::test_default_name);
    RUN_TEST(test_task_builder_defaults::test_default_priority);
    RUN_TEST(test_task_builder_defaults::test_increasing_tid);
    RUN_TEST(test_scheduler_new::test_is_initialized);
    RUN_TEST(test_scheduler_new::test_remove_task);
    RUN_TEST(test_scheduler_new::test_block_unblock);

    RUN_TEST(test_round_robin_locked::test_locked_enqueue_dequeue);
    RUN_TEST(test_round_robin_locked::test_locked_dequeue_middle);
    RUN_TEST(test_round_robin_locked::test_locked_fifo_order);

    RUN_TEST(test_sched_class_hooks::test_task_tick_quantum);
    RUN_TEST(test_sched_class_hooks::test_task_fork_inherits_priority);
    RUN_TEST(test_sched_class_hooks::test_task_deadline_default_zero);

    RUN_TEST(test_priority::test_priority_picks_lowest_value);
    RUN_TEST(test_priority::test_priority_round_robin_within_level);
    RUN_TEST(test_priority::test_priority_ignores_enqueue_order);

    RUN_TEST(test_multi_class::test_pick_next_from_skips_empty);
    RUN_TEST(test_multi_class::test_pick_next_from_all_empty);
    RUN_TEST(test_multi_class::test_pick_next_from_first_class_wins);

    TEST_SUMMARY();
}
