#include <sys/mman.h>

#include "../framework/framework.hpp"
#include "cinux/memory.hpp"
#include "cinux/ptr.hpp"
#include "kernel/arch/x86_64/page.hpp"
#include "kernel/mm/layout.hpp"
#include "kernel/proc/sched_class.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/signals.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/syscall/signal.hpp"
#include "kernel/syscall/sys_signal.hpp"
#include "kernel/syscall/syscall.hpp"
#include "test_assert.hpp"

namespace {

using cinux::proc::DefaultDisposition;
using cinux::proc::DeliverPendingSignals;
using cinux::proc::SendSignal;
using cinux::proc::SignalCatchable;
using cinux::proc::SignalDisposition;
using cinux::proc::SignalTrampolineAddress;
using cinux::proc::TaskState;
using cinux::syscall::SigAction;
using cinux::syscall::SigSet;
using cinux::syscall::SyscallFrame;
using cinux::syscall::kSaResethand;
using cinux::syscall::kSigChld;
using cinux::syscall::kSigCont;
using cinux::syscall::kSigInt;
using cinux::syscall::kSigKill;
using cinux::syscall::kSigStop;
using cinux::syscall::kSigTerm;
using cinux::syscall::kSigTstp;
using cinux::syscall::kSigTtin;
using cinux::syscall::kSigTtou;
using cinux::syscall::kSigUsr1;
using cinux::syscall::kSigUsr2;
using cinux::syscall::kSignalDefault;
using cinux::syscall::kSignalIgnore;

constexpr SigSet bit_for(int number) {
    return SigSet{1ULL << static_cast<unsigned>(number - 1)};
}

/// A delivery writes its context under the interrupted rsp, so the host
/// test plants real memory at a fake rsp inside the kernel's own stack
/// window: the engine's address arithmetic runs against the constants
/// the kernel actually uses. The window is planted at run time: the
/// host process keeps its own live stack up by the window's top end
/// (pinned there under sanitizers), so mapping starts mid-span and
/// steps upward over occupied ground instead of replacing those frames.
unsigned long long      g_fake_rsp   = 0;
constexpr unsigned long kHandlerWord = 0x0000000000401000UL;

struct World;

World* g_world = nullptr;

class ExitSink {
public:
    void switch_tasks(cinux::proc::Task& outgoing, cinux::proc::Task& incoming);
    void enter_from_main(cinux::proc::Task& incoming);
    void return_to_main(cinux::proc::Task& outgoing);
    void reclaim(cinux::proc::Task& dead);

private:
    void run_body_of(cinux::proc::Task& incoming);
};

struct World {
    ExitSink          sink;
    cinux::proc::Task kin;
    cinux::proc::Task alpha;
    cinux::proc::Task beta;
    cinux::proc::Task chief;
    cinux::proc::Task extra_a;
    cinux::proc::Task extra_b;
    SyscallFrame      frame{};
    void (*body)(World&)                = nullptr;
    cinux::proc::Task*        body_task = nullptr;
    bool                      rewrote   = false;
    cinux::proc::ChildReceipt first{};
    cinux::proc::ChildReceipt second{};
    cinux::proc::ChildReceipt third{};
    cinux::proc::ChildReceipt fourth{};
    bool                      first_ok  = false;
    bool                      second_ok = false;
    bool                      third_ok  = false;
    bool                      fourth_ok = false;

    explicit World(unsigned long long slot) {
        kin.name         = "kin";
        alpha.name       = "signal";
        alpha.stack_base = 0x1000ULL * slot;
        alpha.parent     = &kin;
        beta.name        = "signal";
        beta.stack_base  = 0x1000ULL * (slot + 1);
        beta.parent      = &kin;
        body_task        = &alpha;
        cinux::proc::Scheduler::self().init(sink);
        cinux::proc::Scheduler::self().install_class(cinux::proc::DefaultFifoClass());
        cinux::proc::Scheduler::self().seat(alpha);
        cinux::proc::Scheduler::self().seat(beta);
    }

    /// Seats the task the wait scenarios run as; the ready queue hands
    /// out tasks in seat order, so a chief seated after the pair finds
    /// its children already zombies, and one seated before them finds
    /// them alive.
    void seat_chief(unsigned long long group) {
        chief.name   = "chief";
        chief.parent = &kin;
        chief.pgid   = group;
        cinux::proc::Scheduler::self().seat(chief);
    }

    /// Seats two extra children of the named parent under the group
    /// identities the scenario wants; the seats hand out the pids the
    /// waits answer with.
    void seed_pair(cinux::proc::Task& parent, unsigned long long a_group,
                   unsigned long long b_group) {
        extra_a.name   = "pairmate";
        extra_a.parent = &parent;
        extra_a.pgid   = a_group;
        extra_b.name   = "pairmate";
        extra_b.parent = &parent;
        extra_b.pgid   = b_group;
        cinux::proc::Scheduler::self().seat(extra_a);
        cinux::proc::Scheduler::self().seat(extra_b);
    }

    /// One wait from inside the running body task, recorded for the
    /// test body to judge after the dust settles.
    bool wait_into(long long wanted, bool no_hang, cinux::proc::ChildReceipt& into) {
        return cinux::proc::Scheduler::self().wait_child(wanted, no_hang, into);
    }

    void finish() { cinux::proc::Scheduler::self().exit_current(); }

    void run() {
        g_world = this;
        cinux::proc::Scheduler::self().run_until_done();
        g_world = nullptr;
    }

    /// The roll must go back empty: kin enters once and exits, and the
    /// exit detaches every child — zombies reclaimed, halts dropped —
    /// so no list node outlives this stack frame into the next test.
    ~World() {
        g_world = this;
        cinux::proc::Scheduler::self().seat(kin);
        cinux::proc::Scheduler::self().run_until_done();
        g_world = nullptr;
    }
};

void ExitSink::run_body_of(cinux::proc::Task& incoming) {
    if (g_world->body != nullptr && &incoming == g_world->body_task) {
        g_world->body(*g_world);
        return;
    }
    cinux::proc::Scheduler::self().exit_current();
}

void ExitSink::enter_from_main(cinux::proc::Task& incoming) {
    run_body_of(incoming);
}

void ExitSink::switch_tasks(cinux::proc::Task& outgoing, cinux::proc::Task& incoming) {
    static_cast<void>(outgoing);
    run_body_of(incoming);
}

void ExitSink::return_to_main(cinux::proc::Task& outgoing) {
    static_cast<void>(outgoing);
}

void ExitSink::reclaim(cinux::proc::Task& dead) {
    static_cast<void>(dead);
}

bool frame_says(const SyscallFrame& frame, unsigned long long original_rsp) {
    const unsigned long long kContext =
        (original_rsp - 128 - sizeof(cinux::proc::SigReturnContext)) & ~0xFULL;
    const unsigned long long kReturnSlot = kContext - 8;
    const auto* const        kImage      = cinux::base::PtrAt<const cinux::proc::SigReturnContext>(
        static_cast<unsigned long>(kContext));
    const unsigned long long kSavedRsp =
        kImage->user_rsp;  // NOLINT(clang-analyzer-optin.*) the window is mapped in main
    const bool kRegisters = frame.user_rsp == kReturnSlot && frame.rcx == kHandlerWord &&
                            frame.rdi == static_cast<unsigned long long>(kSigInt) && frame.rax == 0;
    const bool kStack = kSavedRsp == original_rsp && kImage->old_blocked.raw == 0 &&
                        *cinux::base::PtrAt<const unsigned long long>(
                            static_cast<unsigned long>(kReturnSlot)) == SignalTrampolineAddress();
    return kRegisters && kStack;
}

TEST("signal tables: the catchable set excludes kill, stop and the outside") {
    ASSERT_TRUE(!SignalCatchable(kSigKill));
    ASSERT_TRUE(!SignalCatchable(kSigStop));
    ASSERT_TRUE(!SignalCatchable(0));
    ASSERT_TRUE(!SignalCatchable(23));
    ASSERT_TRUE(SignalCatchable(kSigInt));
    ASSERT_TRUE(SignalCatchable(kSigTerm));
    ASSERT_TRUE(SignalCatchable(kSigChld));
}

TEST("signal tables: the blockable set answers the same two exceptions") {
    ASSERT_TRUE(!cinux::proc::SignalBlockable(kSigKill));
    ASSERT_TRUE(!cinux::proc::SignalBlockable(kSigStop));
    ASSERT_TRUE(cinux::proc::SignalBlockable(kSigUsr1));
}

TEST("signal tables: terminate and ignore split the default rows") {
    ASSERT_TRUE(DefaultDisposition(kSigInt) == SignalDisposition::kTerminate);
    ASSERT_TRUE(DefaultDisposition(kSigTerm) == SignalDisposition::kTerminate);
    ASSERT_TRUE(DefaultDisposition(kSigKill) == SignalDisposition::kTerminate);
    ASSERT_TRUE(DefaultDisposition(kSigChld) == SignalDisposition::kIgnore);
    ASSERT_TRUE(DefaultDisposition(kSigUsr1) == SignalDisposition::kIgnore);
    ASSERT_TRUE(DefaultDisposition(kSigUsr2) == SignalDisposition::kIgnore);
}

TEST("signal tables: continue and stop own the scheduling rows") {
    ASSERT_TRUE(DefaultDisposition(kSigCont) == SignalDisposition::kContinue);
    ASSERT_TRUE(DefaultDisposition(kSigStop) == SignalDisposition::kStop);
    ASSERT_TRUE(DefaultDisposition(kSigTstp) == SignalDisposition::kStop);
    ASSERT_TRUE(DefaultDisposition(kSigTtin) == SignalDisposition::kStop);
    ASSERT_TRUE(DefaultDisposition(kSigTtou) == SignalDisposition::kStop);
}

TEST("signal send: bad numbers refuse and zombies shrug") {
    cinux::proc::Task target;
    target.state = TaskState::kReady;
    ASSERT_TRUE(!SendSignal(target, 0));
    ASSERT_TRUE(!SendSignal(target, 23));
    ASSERT_TRUE(target.signals.pending.raw == 0);

    target.state = TaskState::kZombie;
    ASSERT_TRUE(SendSignal(target, kSigKill));
    ASSERT_TRUE(target.signals.pending.raw == 0);
}

TEST("signal send: an ignored signal drops, anything else parks a bit") {
    cinux::proc::Task target;
    target.state                                 = TaskState::kReady;
    target.signals.actions[kSigUsr1 - 1].handler = kSignalIgnore;
    ASSERT_TRUE(SendSignal(target, kSigUsr1));
    ASSERT_TRUE(target.signals.pending.raw == 0);

    ASSERT_TRUE(SendSignal(target, kSigInt));
    ASSERT_TRUE(target.signals.pending.has(bit_for(kSigInt)));
}

TEST("signal delivery: an ignored signal evaporates on the return path") {
    World world(700);
    world.body = [](World& site) {
        site.alpha.signals.actions[kSigUsr1 - 1].handler = kSignalIgnore;
        SendSignal(site.alpha, kSigUsr1);
        site.rewrote = DeliverPendingSignals(site.frame);
        cinux::proc::Scheduler::self().exit_current();
    };
    world.run();
    ASSERT_TRUE(!world.rewrote);
    ASSERT_TRUE(world.alpha.signals.pending.raw == 0);
}

TEST("signal delivery: a blocked signal stays parked until the mask opens") {
    World world(800);
    world.body = [](World& site) {
        site.alpha.signals.blocked = bit_for(kSigInt);
        SendSignal(site.alpha, kSigInt);
        site.rewrote = DeliverPendingSignals(site.frame);
        cinux::proc::Scheduler::self().exit_current();
    };
    world.run();
    ASSERT_TRUE(!world.rewrote);
    ASSERT_TRUE(world.alpha.signals.pending.has(bit_for(kSigInt)));
}

TEST("signal delivery: a default terminate exits carrying the number") {
    World world(900);
    world.body = [](World& site) {
        SendSignal(site.alpha, kSigTerm);
        site.rewrote = DeliverPendingSignals(site.frame);
    };
    world.run();
    ASSERT_TRUE(!world.rewrote);
    ASSERT_TRUE(world.alpha.exit_code == kSigTerm);
}

TEST("signal delivery: a custom handler rewrites the frame onto its stack") {
    World world(1000);
    world.body = [](World& site) {
        SigAction& action   = site.alpha.signals.actions[kSigInt - 1];
        action.handler      = kHandlerWord;
        action.mask         = bit_for(kSigUsr1);
        site.frame.user_rsp = g_fake_rsp;
        site.frame.rax      = 7;
        site.frame.rcx      = 0xDEADULL;
        site.frame.rdi      = 99;
        SendSignal(site.alpha, kSigInt);
        site.rewrote = DeliverPendingSignals(site.frame);
        cinux::proc::Scheduler::self().exit_current();
    };
    world.run();
    ASSERT_TRUE(world.rewrote);
    ASSERT_TRUE(frame_says(world.frame, g_fake_rsp));
    ASSERT_TRUE(world.alpha.signals.pending.raw == 0);
    ASSERT_TRUE(world.alpha.signals.blocked.has(bit_for(kSigUsr1)));
    ASSERT_TRUE(!world.alpha.signals.blocked.has(bit_for(kSigInt)));
}

TEST("signal delivery: handler return restores registers, mask and the interrupted red zone") {
    World world(1050);
    world.body = [](World& site) {
        site.alpha.signals.actions[kSigChld - 1].handler = kHandlerWord;
        site.frame.user_rsp                              = g_fake_rsp - 8;
        site.frame.rax                                   = 7;
        site.frame.rcx                                   = 0x401234;
        site.frame.r11                                   = 0x202;
        site.frame.rbx                                   = 0x12345678;
        site.alpha.signals.blocked                       = bit_for(kSigUsr1);
        const SyscallFrame kBefore                       = site.frame;
        auto* const        kRedZone = cinux::base::PtrAt<unsigned char>(site.frame.user_rsp - 128);
        cinux::base::SetBytes(kRedZone, 0xA5, 128);
        SendSignal(site.alpha, kSigChld);
        site.rewrote  = DeliverPendingSignals(site.frame);
        site.first_ok = true;
        for (unsigned int index = 0; index < 128; ++index) {
            site.first_ok = site.first_ok && kRedZone[index] == 0xA5;
        }
        // A normal handler may push below its return address. RET then
        // leaves rsp at the saved context, without a pointer indirection.
        *cinux::base::PtrAt<unsigned long long>(site.frame.user_rsp - 8) = 0xBAD;
        site.frame.user_rsp += 8;
        site.frame.rax            = 15;
        const long long kRestored = cinux::syscall::HandleRtSigreturn(site.frame);
        site.second_ok = kRestored == 7 &&
                         cinux::base::EqualBytes(&site.frame, &kBefore, sizeof(kBefore)) &&
                         site.alpha.signals.blocked.raw == bit_for(kSigUsr1).raw;
        site.finish();
    };
    world.run();
    ASSERT_TRUE(world.rewrote);
    ASSERT_TRUE(world.first_ok);
    ASSERT_TRUE(world.second_ok);
    ASSERT_EQ(world.alpha.exit_code, 0);
}

TEST("signal syscall: musl's four-word action preserves flags, restorer and mask") {
    World world(1075);
    world.body = [](World& site) {
        unsigned long long record[4] = {
            kHandlerWord,
            cinux::syscall::kSaRestart,
            0x402000,
            bit_for(kSigUsr1).raw,
        };
        unsigned long long old[4] = {};
        SyscallFrame       call{};
        call.rdi                = kSigChld;
        call.rsi                = reinterpret_cast<unsigned long long>(record);
        call.rdx                = reinterpret_cast<unsigned long long>(old);
        call.r10                = sizeof(SigSet);
        const long long  kSet   = cinux::syscall::HandleRtSigaction(call);
        const SigAction& action = site.alpha.signals.actions[kSigChld - 1];
        site.first_ok = kSet == 0 && action.handler == record[0] && action.flags == record[1] &&
                        action.restorer == record[2] && action.mask.raw == record[3];
        call.rsi      = 0;
        const long long kGet = cinux::syscall::HandleRtSigaction(call);
        site.second_ok       = kGet == 0 && cinux::base::EqualBytes(record, old, sizeof(record));
        site.finish();
    };
    world.run();
    ASSERT_TRUE(world.first_ok);
    ASSERT_TRUE(world.second_ok);
}

TEST("signal delivery: resethand reverts the action once it has run") {
    World world(1100);
    world.body = [](World& site) {
        SigAction& action   = site.alpha.signals.actions[kSigInt - 1];
        action.handler      = kHandlerWord;
        action.flags        = kSaResethand;
        site.frame.user_rsp = g_fake_rsp;
        SendSignal(site.alpha, kSigInt);
        site.rewrote = DeliverPendingSignals(site.frame);
        cinux::proc::Scheduler::self().exit_current();
    };
    world.run();
    ASSERT_TRUE(world.rewrote);
    ASSERT_TRUE(world.alpha.signals.actions[kSigInt - 1].handler == kSignalDefault);
}

TEST("signal delivery: a delivery that cannot fit the stack window kills") {
    World world(1200);
    world.body = [](World& site) {
        site.alpha.signals.actions[kSigInt - 1].handler = kHandlerWord;
        site.frame.user_rsp                             = cinux::mm::kUserStackBottom - 0x100UL;
        SendSignal(site.alpha, kSigInt);
        site.rewrote = DeliverPendingSignals(site.frame);
    };
    world.run();
    ASSERT_TRUE(!world.rewrote);
    ASSERT_TRUE(world.alpha.exit_code == kSigInt);
}

TEST("signal scheduler: find_by_pid answers for seated tasks and nobody else") {
    World world(1300);
    ASSERT_TRUE(cinux::proc::Scheduler::self().find_by_pid(world.alpha.pid) == &world.alpha);
    ASSERT_TRUE(cinux::proc::Scheduler::self().find_by_pid(world.beta.pid) == &world.beta);
    ASSERT_TRUE(cinux::proc::Scheduler::self().find_by_pid(9999) == nullptr);
}

TEST("signal pairs: an exit marks the parent's pending chld") {
    World world(1400);
    world.run();
    ASSERT_TRUE(world.kin.signals.pending.has(bit_for(kSigChld)));
    ASSERT_TRUE(world.kin.signals.actions[kSigChld - 1].handler == kSignalDefault);
}

TEST("wait groups: wanted zero keeps to the caller's own group") {
    World world(1500);
    world.seed_pair(world.chief, 0, 7777);
    world.seat_chief(0);
    world.chief.pgid   = world.chief.pid;
    world.extra_a.pgid = world.chief.pid;
    world.body_task    = &world.chief;
    world.body         = [](World& site) {
        site.first_ok = site.wait_into(0, false, site.first);
        site.third_ok = site.wait_into(0, true, site.third);
        site.finish();
    };
    world.run();
    ASSERT_TRUE(world.first_ok);
    ASSERT_EQ(world.first.pid, world.extra_a.pid);
    ASSERT_TRUE(!world.third_ok);
}

TEST("wait groups: below minus one names the group at minus wanted") {
    World world(1600);
    world.seed_pair(world.chief, 8888, 7777);
    world.seat_chief(9999);
    world.body_task = &world.chief;
    world.body      = [](World& site) {
        site.first_ok  = site.wait_into(-8888, false, site.first);
        site.third_ok  = site.wait_into(-8888, true, site.third);
        site.second_ok = site.wait_into(-1, false, site.second);
        site.fourth_ok = site.wait_into(-8888, true, site.fourth);
        site.finish();
    };
    world.run();
    ASSERT_TRUE(world.first_ok);
    ASSERT_EQ(world.first.pid, world.extra_a.pid);
    ASSERT_TRUE(!world.third_ok);
    ASSERT_TRUE(world.second_ok);
    ASSERT_EQ(world.second.pid, world.extra_b.pid);
    ASSERT_TRUE(!world.fourth_ok);
}

TEST("wait no-hang: a living child answers zero instead of parking") {
    World world(1700);
    world.seat_chief(5);
    world.seed_pair(world.chief, 5, 5);
    world.body_task = &world.chief;
    world.body      = [](World& site) {
        site.first_ok = site.wait_into(-1, true, site.first);
        site.finish();
    };
    world.run();
    ASSERT_TRUE(world.first_ok);
    ASSERT_EQ(world.first.pid, 0ULL);
}

TEST("stop and cont: a queued task stands stopped the moment stop lands") {
    World world(1800);
    world.body = [](World& site) {
        SendSignal(site.beta, kSigStop);
        site.finish();
    };
    world.run();
    ASSERT_TRUE(world.beta.state == TaskState::kStopped);
    ASSERT_TRUE(world.beta.signals.pending.has(bit_for(kSigStop)));
}

TEST("stop and cont: a continue puts the halted task back on the queue") {
    World world(1900);
    world.body = [](World& site) {
        SendSignal(site.beta, kSigStop);
        SendSignal(site.beta, kSigCont);
        site.finish();
    };
    world.run();
    ASSERT_TRUE(world.beta.state == TaskState::kZombie);
    ASSERT_TRUE(world.beta.signals.pending.has(bit_for(kSigCont)));
    ASSERT_TRUE(world.kin.signals.pending.has(bit_for(kSigChld)));
}

TEST("stop and cont: a continue sweeps the pending stop-class bits") {
    World world(2000);
    world.body = [](World& site) {
        SendSignal(site.beta, kSigStop);
        SendSignal(site.beta, kSigTstp);
        SendSignal(site.beta, kSigTtin);
        SendSignal(site.beta, kSigTtou);
        SendSignal(site.beta, kSigCont);
        site.finish();
    };
    world.run();
    ASSERT_TRUE(!world.beta.signals.pending.has(bit_for(kSigStop)));
    ASSERT_TRUE(!world.beta.signals.pending.has(bit_for(kSigTstp)));
    ASSERT_TRUE(!world.beta.signals.pending.has(bit_for(kSigTtin)));
    ASSERT_TRUE(!world.beta.signals.pending.has(bit_for(kSigTtou)));
    ASSERT_TRUE(world.beta.signals.pending.has(bit_for(kSigCont)));
    ASSERT_TRUE(world.beta.state == TaskState::kZombie);
}

TEST("stop and cont: kill wakes the halted task, a plain term stays parked") {
    World world(2100);
    world.body = [](World& site) {
        SendSignal(site.beta, kSigStop);
        SendSignal(site.beta, kSigTerm);
        SendSignal(site.beta, kSigKill);
        site.finish();
    };
    world.run();
    ASSERT_TRUE(world.beta.signals.pending.has(bit_for(kSigTerm)));
    ASSERT_TRUE(world.beta.signals.pending.has(bit_for(kSigKill)));
    ASSERT_TRUE(world.beta.state == TaskState::kZombie);
}

TEST("stop delivery: the halting stop consumes itself and halts mid-delivery") {
    World world(2200);
    world.body = [](World& site) {
        SendSignal(site.alpha, kSigStop);
        site.rewrote = DeliverPendingSignals(site.frame);
    };
    world.run();
    ASSERT_TRUE(!world.rewrote);
    ASSERT_TRUE(world.alpha.signals.pending.raw == 0);
    ASSERT_TRUE(world.alpha.state == TaskState::kStopped);
    ASSERT_TRUE(world.beta.state == TaskState::kZombie);
}

}  // namespace

int main() {
    constexpr unsigned long kWindowBytes = 2UL * cinux::arch::page::kSize;
    constexpr unsigned long kStep        = 64UL * 1024UL;
    constexpr unsigned long kKeepClear   = 64UL * 1024UL;
    for (unsigned long top = cinux::mm::kUserStackBottom + kStep;
         top <= cinux::mm::kUserStackTop - kKeepClear; top += kStep) {
        void* const kWindow = cinux::base::PtrAt<void>(top - kWindowBytes);
        if (mmap(kWindow, kWindowBytes, PROT_READ | PROT_WRITE,
                 MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == kWindow) {
            g_fake_rsp = static_cast<unsigned long long>(top - 0x400UL);
            return cinux::test::RunAll();
        }
    }
    return 3;
}
