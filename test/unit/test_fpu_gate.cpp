#include "../framework/framework.hpp"
#include "kernel/arch/x86_64/context.hpp"
#include "kernel/proc/fpu_gate.hpp"
#include "kernel/proc/sched_class.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/task.hpp"
#include "test_assert.hpp"

namespace {

constexpr unsigned long      kXmmWord     = 20;
constexpr unsigned long long kFirstUser   = 0xA11CE00000000001ULL;
constexpr unsigned long long kSecondUser  = 0xB0B0000000000002ULL;
constexpr unsigned long long kKernelValue = 0xC0DE000000000003ULL;

void set_live_value(unsigned long long value) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    cinux::arch::FpuImage image;
    cinux::arch::SaveFpu(image);
    image.words[kXmmWord] = value;
    cinux::arch::RestoreFpu(image);
}

unsigned long long live_value() {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    cinux::arch::FpuImage image;
    cinux::arch::SaveFpu(image);
    return image.words[kXmmWord];
}

enum class Scenario : unsigned char {
    kNested,
    kSwitch,
    kExit,
};

struct GateWorld {
    cinux::proc::Task first;
    cinux::proc::Task second;
    Scenario          scenario;
    unsigned int      first_visits       = 0;
    bool              outer_saved        = false;
    bool              nested_kept        = false;
    bool              inner_exit_kept    = false;
    bool              outer_restored     = false;
    bool              second_independent = false;
    bool              second_restored    = false;

    explicit GateWorld(Scenario chosen) : scenario(chosen) {
        cinux::arch::SaveFpu(first.ctx.fpu);
        cinux::arch::SaveFpu(second.ctx.fpu);
        first.ctx.fpu.words[kXmmWord]  = kFirstUser;
        second.ctx.fpu.words[kXmmWord] = kSecondUser;
        cinux::proc::Scheduler::self().init(*this);
        cinux::proc::Scheduler::self().install_class(cinux::proc::DefaultFifoClass());
        cinux::proc::Scheduler::self().seat(first);
        if (scenario != Scenario::kNested) {
            cinux::proc::Scheduler::self().seat(second);
        }
    }

    void enter_from_main(cinux::proc::Task& incoming) {
        cinux::arch::RestoreFpu(incoming.ctx.fpu);
        run(incoming);
    }

    void switch_tasks(cinux::proc::Task& outgoing, cinux::proc::Task& incoming) {
        cinux::arch::SaveFpu(outgoing.ctx.fpu);
        cinux::arch::RestoreFpu(incoming.ctx.fpu);
        run(incoming);
    }

    void return_to_main(cinux::proc::Task& outgoing) { cinux::arch::SaveFpu(outgoing.ctx.fpu); }

    void reclaim([[maybe_unused]] cinux::proc::Task& dead) {}

    void finish_first() {
        ExitFpuGate();
        inner_exit_kept = first.fpu_gate_depth == 1;
        ExitFpuGate();
        outer_restored = first.fpu_gate_depth == 0 && first.entry_fpu.words[kXmmWord] == kFirstUser;
        cinux::proc::Scheduler::self().exit_current();
    }

    void run(cinux::proc::Task& task) {
        auto& scheduler = cinux::proc::Scheduler::self();
        if (&task == &second) {
            EnterFpuGate();
            second_independent =
                second.fpu_gate_depth == 1 && second.entry_fpu.words[kXmmWord] == kSecondUser;
            set_live_value(kKernelValue);
            ExitFpuGate();
            second_restored =
                second.fpu_gate_depth == 0 && second.entry_fpu.words[kXmmWord] == kSecondUser;
            scheduler.exit_current();
            return;
        }
        if (first_visits++ != 0) {
            finish_first();
            return;
        }
        EnterFpuGate();
        outer_saved = first.fpu_gate_depth == 1 && first.entry_fpu.words[kXmmWord] == kFirstUser;
        set_live_value(kKernelValue);
        EnterFpuGate();
        nested_kept = first.fpu_gate_depth == 2 && first.entry_fpu.words[kXmmWord] == kFirstUser;
        if (scenario == Scenario::kSwitch) {
            scheduler.yield();
        } else if (scenario == Scenario::kExit) {
            scheduler.exit_current();
        } else {
            finish_first();
        }
    }
};

// NOLINTNEXTLINE(misc-const-correctness)
void run_world(GateWorld& world) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    cinux::arch::FpuImage original;
    cinux::arch::SaveFpu(original);
    cinux::proc::Scheduler::self().run_until_done();
    cinux::arch::RestoreFpu(original);
    ASSERT_TRUE(world.outer_saved);
    ASSERT_TRUE(world.nested_kept);
    ASSERT_TRUE(cinux::proc::Scheduler::self().current() == nullptr);
}

TEST("fpu gate: entries before scheduling leave the live image alone") {
    const unsigned long long kBefore = live_value();
    EnterFpuGate();
    ExitFpuGate();
    ASSERT_EQ(live_value(), kBefore);
}

TEST("fpu gate: only the outer exit consumes the task's own entry image") {
    GateWorld world(Scenario::kNested);
    run_world(world);
    ASSERT_TRUE(world.inner_exit_kept);
    ASSERT_TRUE(world.outer_restored);
}

TEST("fpu gate: tasks switched inside nested gates keep independent images and depths") {
    GateWorld world(Scenario::kSwitch);
    run_world(world);
    ASSERT_TRUE(world.second_independent);
    ASSERT_TRUE(world.second_restored);
    ASSERT_TRUE(world.inner_exit_kept);
    ASSERT_TRUE(world.outer_restored);
}

TEST("fpu gate: exiting without unwinding cannot strand the next task inside a gate") {
    GateWorld world(Scenario::kExit);
    run_world(world);
    ASSERT_TRUE(world.second_independent);
    ASSERT_TRUE(world.second_restored);
}

}  // namespace

int main() {
    return cinux::test::RunAll();
}
