#include "../framework/framework.hpp"
#include "kernel/proc/proc_config.hpp"
#include "kernel/proc/sched_class.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/task.hpp"
#include "test_assert.hpp"

namespace {

enum class Action : unsigned char {
    kEnter,
    kSwitch,
    kReturnMain,
    kReclaim,
};

struct Recorded {
    Action             action;
    unsigned long long from_pid;
    unsigned long long to_pid;
};

struct World;

World* g_world = nullptr;

class RecordingSink {
public:
    void switch_tasks(cinux::proc::Task& outgoing, cinux::proc::Task& incoming);
    void enter_from_main(cinux::proc::Task& incoming);
    void return_to_main(cinux::proc::Task& outgoing);
    void reclaim(cinux::proc::Task& dead) {
        record(Action::kReclaim, dead.pid, 0);
        reclaimed_count++;
    }

    Recorded     log[16]{};
    unsigned int played          = 0;
    unsigned int reclaimed_count = 0;

private:
    void record(Action action, unsigned long long from_pid, unsigned long long to_pid) {
        if (played < 16) {
            log[played++] = Recorded{.action = action, .from_pid = from_pid, .to_pid = to_pid};
        }
    }
};

struct World {
    RecordingSink     sink;
    cinux::proc::Task alpha;
    cinux::proc::Task beta;
    unsigned int      alpha_visits = 0;
    bool              clock_mode   = false;

    explicit World(unsigned long long slot) {
        alpha.name       = "host";
        alpha.stack_base = 0x1000ULL * slot;
        beta.name        = "host";
        beta.stack_base  = 0x1000ULL * (slot + 1);
        cinux::proc::Scheduler::self().init(sink);
        cinux::proc::Scheduler::self().install_class(cinux::proc::DefaultFifoClass());
        cinux::proc::Scheduler::self().seat(alpha);
        cinux::proc::Scheduler::self().seat(beta);
    }

    ~World() {
        g_world = this;
        cinux::proc::Scheduler::self().run_until_done();
        g_world = nullptr;
    }

    void run_body_of(cinux::proc::Task& task) {
        if (clock_mode) {
            run_clock_body(task);
            return;
        }
        if (&task == &alpha) {
            alpha_visits++;
            if (alpha_visits == 1) {
                cinux::proc::Scheduler::self().yield();
                return;
            }
            cinux::proc::Scheduler::self().exit_current();
            return;
        }
        cinux::proc::Scheduler::self().exit_current();
    }

    void run_clock_body(cinux::proc::Task& task) {
        if (&task == &alpha) {
            alpha_visits++;
            if (alpha_visits > 1) {
                cinux::proc::Scheduler::self().exit_current();
                return;
            }
            auto& scheduler = cinux::proc::Scheduler::self();
            scheduler.set_preemption(true);
            for (unsigned int tick = 0; tick < cinux::proc::kTimeSliceTicks; ++tick) {
                scheduler.on_timer_tick();
            }
            scheduler.maybe_preempt();
            scheduler.set_preemption(false);
            return;
        }
        cinux::proc::Scheduler::self().exit_current();
    }
};

void RecordingSink::enter_from_main(cinux::proc::Task& incoming) {
    record(Action::kEnter, 0, incoming.pid);
    g_world->run_body_of(incoming);
}

void RecordingSink::switch_tasks(cinux::proc::Task& outgoing, cinux::proc::Task& incoming) {
    record(Action::kSwitch, outgoing.pid, incoming.pid);
    g_world->run_body_of(incoming);
}

void RecordingSink::return_to_main(cinux::proc::Task& outgoing) {
    record(Action::kReturnMain, outgoing.pid, 0);
}

TEST("scheduler: seat hands out consecutive pids and parks ready") {
    const World kWorld(100);
    ASSERT_TRUE(kWorld.beta.pid == kWorld.alpha.pid + 1);
    ASSERT_TRUE(kWorld.alpha.state == cinux::proc::TaskState::kReady);
    ASSERT_TRUE(kWorld.beta.state == cinux::proc::TaskState::kReady);
}

TEST("scheduler: drain enters the queue in fifo order") {
    World world(200);
    g_world = &world;
    cinux::proc::Scheduler::self().run_until_done();
    ASSERT_TRUE(world.sink.played >= 1);
    ASSERT_TRUE(world.sink.log[0].action == Action::kEnter);
    ASSERT_TRUE(world.sink.log[0].to_pid == world.alpha.pid);
    ASSERT_TRUE(cinux::proc::Scheduler::self().current() == nullptr);
}

void check_step(const Recorded& step, Action action, unsigned long long from_pid,
                unsigned long long to_pid) {
    ASSERT_TRUE(step.action == action);
    ASSERT_TRUE(step.from_pid == from_pid);
    ASSERT_TRUE(step.to_pid == to_pid);
}

TEST("scheduler: yield alternates, exit drains, both reclaims run") {
    World world(300);
    g_world = &world;
    cinux::proc::Scheduler::self().run_until_done();

    ASSERT_TRUE(world.alpha_visits == 2);
    ASSERT_TRUE(world.sink.played == 6);
    check_step(world.sink.log[0], Action::kEnter, 0, world.alpha.pid);
    check_step(world.sink.log[1], Action::kSwitch, world.alpha.pid, world.beta.pid);
    check_step(world.sink.log[2], Action::kSwitch, world.beta.pid, world.alpha.pid);
    check_step(world.sink.log[3], Action::kReclaim, world.beta.pid, 0);
    check_step(world.sink.log[4], Action::kReturnMain, world.alpha.pid, 0);
    check_step(world.sink.log[5], Action::kReclaim, world.alpha.pid, 0);
    ASSERT_TRUE(world.sink.reclaimed_count == 2);
    ASSERT_TRUE(world.alpha.state == cinux::proc::TaskState::kDead);
    ASSERT_TRUE(world.beta.state == cinux::proc::TaskState::kDead);
}

TEST("scheduler: the exit hook rotates a task the clock flagged") {
    World world(400);
    world.clock_mode =
        true;  // NOLINT(misc-const-correctness) the sink mutates it through the world
    g_world = &world;
    cinux::proc::Scheduler::self().run_until_done();

    ASSERT_TRUE(world.sink.played >= 2);
    ASSERT_TRUE(world.sink.log[0].action == Action::kEnter);
    ASSERT_TRUE(world.sink.log[0].to_pid == world.alpha.pid);
    ASSERT_TRUE(world.sink.log[1].action == Action::kSwitch);
    ASSERT_TRUE(world.sink.log[1].from_pid == world.alpha.pid);
    ASSERT_TRUE(world.sink.log[1].to_pid == world.beta.pid);
    ASSERT_TRUE(world.alpha_visits == 2);
    ASSERT_TRUE(world.sink.reclaimed_count == 2);
}

}  // namespace

int main() {
    return cinux::test::RunAll();
}
