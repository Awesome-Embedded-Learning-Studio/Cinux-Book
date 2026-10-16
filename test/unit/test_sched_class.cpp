#include <string_view>

#include "../framework/framework.hpp"
#include "kernel/proc/proc_config.hpp"
#include "kernel/proc/sched_class.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/task.hpp"
#include "test_assert.hpp"

namespace {

constexpr unsigned int kRosterLimit = cinux::proc::kReadyQueueCapacity + 1;

struct VisitWorld;

VisitWorld* g_visit_world = nullptr;

class VisitSink {
public:
    void enter_from_main(cinux::proc::Task& incoming);
    void switch_tasks([[maybe_unused]] cinux::proc::Task& outgoing, cinux::proc::Task& incoming);
    void return_to_main([[maybe_unused]] cinux::proc::Task& outgoing) {}
    void reclaim([[maybe_unused]] cinux::proc::Task& dead) {}
};

struct VisitWorld {
    VisitSink          sink{};
    cinux::proc::Task  plain{};
    cinux::proc::Task  urgent{};
    unsigned long long visits[8]{};
    unsigned int       visit_count     = 0;
    unsigned int       plain_visits    = 0;
    bool               slice_mode      = false;
    bool               kept_full_slice = false;

    VisitWorld() {
        plain.stack_base  = 0x51000ULL;
        urgent.stack_base = 0x52000ULL;
        auto& scheduler   = cinux::proc::Scheduler::self();
        scheduler.init(sink);
        scheduler.install_class(cinux::proc::DefaultFifoClass());
        scheduler.install_class(cinux::proc::PriorityClass());
    }

    void run_body_of(cinux::proc::Task& task) {
        if (slice_mode) {
            run_slice_body(task);
            return;
        }
        if (visit_count < 8) {
            visits[visit_count++] = task.pid;
        }
        cinux::proc::Scheduler::self().exit_current();
    }

    void run_slice_body(cinux::proc::Task& task) {
        auto& scheduler = cinux::proc::Scheduler::self();
        if (&task == &plain) {
            plain_visits++;
            if (plain_visits > 1) {
                scheduler.exit_current();
                return;
            }
            for (unsigned int tick = 1; tick < cinux::proc::kTimeSliceTicks; ++tick) {
                scheduler.on_timer_tick();
            }
            scheduler.yield();
            return;
        }
        scheduler.on_timer_tick();
        scheduler.maybe_preempt();
        kept_full_slice = scheduler.current() == &urgent;
        scheduler.exit_current();
    }
};

void VisitSink::enter_from_main(cinux::proc::Task& incoming) {
    g_visit_world->run_body_of(incoming);
}

void VisitSink::switch_tasks([[maybe_unused]] cinux::proc::Task& outgoing,
                             cinux::proc::Task&                  incoming) {
    g_visit_world->run_body_of(incoming);
}

TEST("sched class: the registry keeps classes in registration order, idempotently") {
    auto& scheduler = cinux::proc::Scheduler::self();
    ASSERT_TRUE(scheduler.class_count() == 0);
    scheduler.install_class(cinux::proc::DefaultFifoClass());
    scheduler.install_class(cinux::proc::DefaultFifoClass());
    ASSERT_TRUE(scheduler.class_count() == 1);
    scheduler.install_class(cinux::proc::PriorityClass());
    ASSERT_TRUE(scheduler.class_count() == 2);
    ASSERT_TRUE(scheduler.class_at(0) == &cinux::proc::DefaultFifoClass());
    ASSERT_TRUE(scheduler.class_at(1) == &cinux::proc::PriorityClass());
    ASSERT_TRUE(scheduler.class_at(2) == nullptr);
}

TEST("sched class: the fifo class hands tasks back in arrival order") {
    cinux::proc::Task             first;
    cinux::proc::Task             second;
    cinux::proc::Task             third;
    cinux::proc::SchedulingClass& fifo = cinux::proc::DefaultFifoClass();

    ASSERT_TRUE(fifo.enqueue(first));
    ASSERT_TRUE(fifo.enqueue(second));
    ASSERT_TRUE(fifo.enqueue(third));
    ASSERT_TRUE(fifo.pick() == &first);
    ASSERT_TRUE(fifo.pick() == &second);
    ASSERT_TRUE(fifo.pick() == &third);
    ASSERT_TRUE(fifo.pick() == nullptr);
}

TEST("sched class: the priority class takes the smallest weight first, ties by age") {
    cinux::proc::Task             oldest;
    cinux::proc::Task             middle;
    cinux::proc::Task             urgent;
    cinux::proc::SchedulingClass& priority = cinux::proc::PriorityClass();

    oldest.priority = 5;
    middle.priority = 5;
    urgent.priority = 1;
    ASSERT_TRUE(priority.enqueue(oldest));
    ASSERT_TRUE(priority.enqueue(middle));
    ASSERT_TRUE(priority.enqueue(urgent));
    ASSERT_TRUE(priority.pick() == &urgent);
    ASSERT_TRUE(priority.pick() == &oldest);
    ASSERT_TRUE(priority.pick() == &middle);
    ASSERT_TRUE(priority.pick() == nullptr);
}

TEST("sched class: a full class queue refuses honestly") {
    static cinux::proc::Task      crowd[kRosterLimit];
    cinux::proc::SchedulingClass& fifo   = cinux::proc::DefaultFifoClass();
    unsigned int                  seated = 0;
    for (auto& task : crowd) {
        if (fifo.enqueue(task)) {
            seated++;
        }
    }
    ASSERT_TRUE(seated == cinux::proc::kReadyQueueCapacity);
    for (unsigned int i = 0; i < cinux::proc::kReadyQueueCapacity; ++i) {
        ASSERT_TRUE(fifo.pick() != nullptr);
    }
    ASSERT_TRUE(fifo.pick() == nullptr);
}

TEST("sched class: both classes spend the standard slice before rotating") {
    const unsigned int kSlice     = cinux::proc::kTimeSliceTicks;
    const bool         kFifoEarly = cinux::proc::DefaultFifoClass().wants_rotate(kSlice - 1);
    const bool         kPrioEarly = cinux::proc::PriorityClass().wants_rotate(kSlice - 1);
    ASSERT_TRUE(!kFifoEarly);
    ASSERT_TRUE(cinux::proc::DefaultFifoClass().wants_rotate(kSlice));
    ASSERT_TRUE(!kPrioEarly);
    ASSERT_TRUE(cinux::proc::PriorityClass().wants_rotate(kSlice));
}

TEST("sched class: the classes spell their names for the dumps") {
    ASSERT_TRUE(std::string_view(cinux::proc::DefaultFifoClass().name()) ==
                std::string_view("fifo"));
    ASSERT_TRUE(std::string_view(cinux::proc::PriorityClass().name()) ==
                std::string_view("priority"));
}

TEST("sched class: the first registered class empties before the second is asked") {
    VisitWorld world{};
    auto&      scheduler = cinux::proc::Scheduler::self();
    ASSERT_TRUE(scheduler.class_at(0) == &cinux::proc::DefaultFifoClass());
    ASSERT_TRUE(scheduler.class_at(1) == &cinux::proc::PriorityClass());
    world.urgent.priority    = 1;
    world.urgent.sched_class = &cinux::proc::PriorityClass();
    g_visit_world            = &world;
    scheduler.seat(world.plain);
    scheduler.seat(world.urgent);
    scheduler.run_until_done();
    g_visit_world = nullptr;
    ASSERT_TRUE(world.visit_count == 2);
    ASSERT_TRUE(world.visits[0] == world.plain.pid);
    ASSERT_TRUE(world.visits[1] == world.urgent.pid);
}

TEST("sched class: a handed-off task earns a fresh full slice") {
    VisitWorld world{};
    auto&      scheduler = cinux::proc::Scheduler::self();
    g_visit_world        = &world;
    world.slice_mode     = true;
    scheduler.set_preemption(true);
    scheduler.seat(world.plain);
    scheduler.seat(world.urgent);
    scheduler.run_until_done();
    scheduler.set_preemption(false);
    g_visit_world = nullptr;
    ASSERT_TRUE(world.kept_full_slice);
    ASSERT_TRUE(world.plain_visits == 2);
}

}  // namespace

int main() {
    return cinux::test::RunAll();
}
