/**
 * @file kernel/test/test_signal.cpp
 * @brief QEMU in-kernel tests for the signal data layer (F3-M1 batch 1)
 *
 * Covers the bitmask signal-set operations, the default-disposition table,
 * the uncatchable predicate, the validity check, and that the per-Task
 * signal fields default to a clean state (fork relies on this).  Delivery
 * and the syscall surface arrive in later batches.
 */

#include <stdint.h>

#include "big_kernel_test.h"
#include "kernel/arch/x86_64/idt.hpp"
#include "kernel/proc/process.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/signal.hpp"
#include "kernel/syscall/sys_signal.hpp"

using namespace cinux::proc;
using cinux::syscall::UserSigAction;
using cinux::syscall::sys_kill;
using cinux::syscall::sys_rt_sigaction;
using cinux::syscall::sys_rt_sigprocmask;

namespace {

/// RAII: install @p task as current, restore the previous on destruction.
struct CurrentTaskGuard {
    cinux::proc::Task* prev;
    explicit CurrentTaskGuard(cinux::proc::Task* task) : prev(cinux::proc::Scheduler::current()) {
        cinux::proc::Scheduler::set_current(task);
    }
    ~CurrentTaskGuard() { cinux::proc::Scheduler::set_current(prev); }
};

}  // namespace

// ============================================================
// Signal-set bitmask operations
// ============================================================

namespace test_sigset {

void test_set_add_del_member() {
    SigSet s = 0;
    sig_set_add(s, Signal::kSigint);
    sig_set_add(s, Signal::kSigterm);
    TEST_ASSERT_TRUE(sig_is_member(s, Signal::kSigint));
    TEST_ASSERT_TRUE(sig_is_member(s, Signal::kSigterm));
    TEST_ASSERT_FALSE(sig_is_member(s, Signal::kSighup));

    sig_set_del(s, Signal::kSigint);
    TEST_ASSERT_FALSE(sig_is_member(s, Signal::kSigint));
    TEST_ASSERT_TRUE(sig_is_member(s, Signal::kSigterm));
}

void test_make_mask_bit_position() {
    // bit n <=> signal n: signal 9 (SIGKILL) sets bit 9, signal 1 sets bit 1.
    TEST_ASSERT_EQ(sig_make_mask(Signal::kSigkill), SigSet{1} << 9);
    TEST_ASSERT_EQ(sig_make_mask(Signal::kSighup), SigSet{1} << 1);
}

}  // namespace test_sigset

// ============================================================
// Default disposition table
// ============================================================

namespace test_sig_default {

void test_default_actions() {
    TEST_ASSERT_EQ(signal_default_action(Signal::kSigterm), SigDefault::kTerminate);
    TEST_ASSERT_EQ(signal_default_action(Signal::kSighup), SigDefault::kTerminate);
    TEST_ASSERT_EQ(signal_default_action(Signal::kSigpipe), SigDefault::kTerminate);

    TEST_ASSERT_EQ(signal_default_action(Signal::kSigquit), SigDefault::kCoreDump);
    TEST_ASSERT_EQ(signal_default_action(Signal::kSigsegv), SigDefault::kCoreDump);
    TEST_ASSERT_EQ(signal_default_action(Signal::kSigfpe), SigDefault::kCoreDump);

    TEST_ASSERT_EQ(signal_default_action(Signal::kSigchld), SigDefault::kIgnore);

    TEST_ASSERT_EQ(signal_default_action(Signal::kSigcont), SigDefault::kContinue);

    TEST_ASSERT_EQ(signal_default_action(Signal::kSigstop), SigDefault::kStop);
    TEST_ASSERT_EQ(signal_default_action(Signal::kSigtstp), SigDefault::kStop);
}

}  // namespace test_sig_default

// ============================================================
// Uncatchable / validity
// ============================================================

namespace test_sig_meta {

void test_uncatchable() {
    TEST_ASSERT_TRUE(signal_is_uncatchable(Signal::kSigkill));
    TEST_ASSERT_TRUE(signal_is_uncatchable(Signal::kSigstop));
    TEST_ASSERT_FALSE(signal_is_uncatchable(Signal::kSigterm));
    TEST_ASSERT_FALSE(signal_is_uncatchable(Signal::kSigint));
}

void test_valid() {
    TEST_ASSERT_TRUE(signal_valid(1));
    TEST_ASSERT_TRUE(signal_valid(static_cast<int>(Signal::kSigtou)));
    TEST_ASSERT_FALSE(signal_valid(0));
    TEST_ASSERT_FALSE(signal_valid(static_cast<int>(Signal::kSigtou) + 1));
}

}  // namespace test_sig_meta

// ============================================================
// SigAction / Task wiring
// ============================================================

namespace test_sig_state {

void test_sigaction_default_ctor() {
    SigAction a;
    TEST_ASSERT_EQ(a.type, HandlerType::kDefault);
    TEST_ASSERT_EQ(a.handler_addr, uint64_t{0});
    TEST_ASSERT_EQ(a.sa_mask, SigSet{0});
}

void test_task_has_signal_fields() {
    // A value-initialised Task starts with no pending/blocked signals and
    // every disposition at default -- fork relies on this baseline.
    Task t{};
    t.sig_actions = SharedSigActions::create();
    TEST_ASSERT_EQ(t.sig_pending, SigSet{0});
    TEST_ASSERT_EQ(t.sig_blocked, SigSet{0});
    TEST_ASSERT_EQ(t.sig_actions->actions[static_cast<int>(Signal::kSigterm)].type,
                   HandlerType::kDefault);
}

}  // namespace test_sig_state

// ============================================================
// Delivery: send / pick / syscalls (F3-M1 batch 2)
// ============================================================

namespace test_sig_send {

void test_send_sets_pending() {
    Task t{};
    t.sig_actions = SharedSigActions::create();
    CurrentTaskGuard guard(&t);
    t.pid = 1;
    TEST_ASSERT_EQ(signal_send(&t, Signal::kSigterm), 0);
    TEST_ASSERT_TRUE(sig_is_member(t.sig_pending, Signal::kSigterm));
}

void test_send_ignored_is_discarded() {
    Task t{};
    t.sig_actions = SharedSigActions::create();
    CurrentTaskGuard guard(&t);
    t.pid                                                           = 1;
    t.sig_actions->actions[static_cast<int>(Signal::kSigchld)].type = HandlerType::kIgnore;
    TEST_ASSERT_EQ(signal_send(&t, Signal::kSigchld), 0);
    TEST_ASSERT_FALSE(sig_is_member(t.sig_pending, Signal::kSigchld));  // discarded
}

void test_send_uncatchable_overrides_ignore() {
    Task t{};
    t.sig_actions = SharedSigActions::create();
    CurrentTaskGuard guard(&t);
    t.pid                                                           = 1;
    t.sig_actions->actions[static_cast<int>(Signal::kSigkill)].type = HandlerType::kIgnore;
    TEST_ASSERT_EQ(signal_send(&t, Signal::kSigkill), 0);
    TEST_ASSERT_TRUE(sig_is_member(t.sig_pending, Signal::kSigkill));  // SIGKILL not ignored
}

}  // namespace test_sig_send

namespace test_sig_pick {

void test_pick_clears_and_respects_block() {
    Task t{};
    t.sig_actions = SharedSigActions::create();
    CurrentTaskGuard guard(&t);
    sig_set_add(t.sig_pending, Signal::kSigterm);
    sig_set_add(t.sig_blocked, Signal::kSigterm);  // blocked
    TEST_ASSERT_EQ(signal_pick_deliverable(&t), 0);
    TEST_ASSERT_TRUE(sig_is_member(t.sig_pending, Signal::kSigterm));  // still pending

    sig_set_del(t.sig_blocked, Signal::kSigterm);  // unblock
    TEST_ASSERT_EQ(signal_pick_deliverable(&t), static_cast<int>(Signal::kSigterm));
    TEST_ASSERT_FALSE(sig_is_member(t.sig_pending, Signal::kSigterm));  // cleared
}

void test_pick_skips_custom() {
    Task t{};
    t.sig_actions = SharedSigActions::create();
    CurrentTaskGuard guard(&t);
    sig_set_add(t.sig_pending, Signal::kSigterm);
    t.sig_actions->actions[static_cast<int>(Signal::kSigterm)].type = HandlerType::kCustom;
    // Custom handlers need a signal frame (batch 3); left pending.
    TEST_ASSERT_EQ(signal_pick_deliverable(&t), 0);
    TEST_ASSERT_TRUE(sig_is_member(t.sig_pending, Signal::kSigterm));
}

}  // namespace test_sig_pick

namespace test_sig_syscall {

void test_sigaction_install_and_query() {
    Task t{};
    t.sig_actions = SharedSigActions::create();
    CurrentTaskGuard guard(&t);
    UserSigAction    act{};
    act.sa_handler = 0xDEADBEEFu;
    act.sa_mask    = sig_make_mask(Signal::kSigint);
    TEST_ASSERT_EQ(sys_rt_sigaction(static_cast<uint64_t>(Signal::kSigusr1),
                                    reinterpret_cast<uint64_t>(&act), 0, 0, 0, 0),
                   0);
    const SigAction& installed = t.sig_actions->actions[static_cast<int>(Signal::kSigusr1)];
    TEST_ASSERT_EQ(installed.type, HandlerType::kCustom);
    TEST_ASSERT_EQ(installed.handler_addr, uint64_t{0xDEADBEEF});

    UserSigAction old{};
    TEST_ASSERT_EQ(sys_rt_sigaction(static_cast<uint64_t>(Signal::kSigusr1), 0,
                                    reinterpret_cast<uint64_t>(&old), 0, 0, 0),
                   0);
    TEST_ASSERT_EQ(old.sa_handler, uint64_t{0xDEADBEEF});
}

void test_sigaction_rejects_uncatchable() {
    Task t{};
    t.sig_actions = SharedSigActions::create();
    CurrentTaskGuard guard(&t);
    UserSigAction    act{};
    act.sa_handler = 0xDEADBEEFu;
    TEST_ASSERT_EQ(sys_rt_sigaction(static_cast<uint64_t>(Signal::kSigkill),
                                    reinterpret_cast<uint64_t>(&act), 0, 0, 0, 0),
                   -22);  // EINVAL: cannot catch SIGKILL
}

void test_sigprocmask_block_unblock() {
    Task t{};
    t.sig_actions = SharedSigActions::create();
    CurrentTaskGuard guard(&t);
    uint64_t         mask = sig_make_mask(Signal::kSigterm);
    TEST_ASSERT_EQ(
        sys_rt_sigprocmask(0 /*SIG_BLOCK*/, reinterpret_cast<uint64_t>(&mask), 0, 0, 0, 0), 0);
    TEST_ASSERT_TRUE(sig_is_member(t.sig_blocked, Signal::kSigterm));
    TEST_ASSERT_EQ(
        sys_rt_sigprocmask(1 /*SIG_UNBLOCK*/, reinterpret_cast<uint64_t>(&mask), 0, 0, 0, 0), 0);
    TEST_ASSERT_FALSE(sig_is_member(t.sig_blocked, Signal::kSigterm));
}

void test_sigprocmask_cannot_block_sigkill() {
    Task t{};
    t.sig_actions = SharedSigActions::create();
    CurrentTaskGuard guard(&t);
    uint64_t         mask = sig_make_mask(Signal::kSigkill);
    sys_rt_sigprocmask(0, reinterpret_cast<uint64_t>(&mask), 0, 0, 0, 0);
    TEST_ASSERT_FALSE(sig_is_member(t.sig_blocked, Signal::kSigkill));
}

void test_kill_self_pends() {
    Task t{};
    t.sig_actions = SharedSigActions::create();
    CurrentTaskGuard guard(&t);
    t.pid = 42;
    TEST_ASSERT_EQ(sys_kill(0, static_cast<uint64_t>(Signal::kSigusr2), 0, 0, 0, 0), 0);
    TEST_ASSERT_TRUE(sig_is_member(t.sig_pending, Signal::kSigusr2));
    TEST_ASSERT_EQ(sys_kill(42, static_cast<uint64_t>(Signal::kSighup), 0, 0, 0, 0), 0);
    TEST_ASSERT_TRUE(sig_is_member(t.sig_pending, Signal::kSighup));
}

void test_kill_via_registry() {
    // F3-M1 batch 4: sys_kill resolves a target pid through the global task
    // registry rather than the current task.
    Task t1{};  // current (the killer)
    Task t2{};  // target (registered, distinct pid)
    t1.sig_actions = SharedSigActions::create();
    t2.sig_actions = SharedSigActions::create();
    CurrentTaskGuard guard(&t1);
    t1.pid   = 1;
    t2.pid   = 100;
    t2.state = TaskState::Running;
    signal_register_task(&t2);
    TEST_ASSERT_EQ(sys_kill(100, static_cast<uint64_t>(Signal::kSigusr1), 0, 0, 0, 0), 0);
    TEST_ASSERT_TRUE(sig_is_member(t2.sig_pending, Signal::kSigusr1));   // target hit
    TEST_ASSERT_FALSE(sig_is_member(t1.sig_pending, Signal::kSigusr1));  // killer untouched
    signal_unregister_task(&t2);
}

}  // namespace test_sig_syscall

// ============================================================
// Signal frame & sigreturn (F3-M1 batch 3)
// ============================================================

namespace test_sig_frame {

void test_setup_frame_redirects_to_handler() {
    using cinux::arch::InterruptFrame;
    static uint8_t stack_buf[4096];
    const uint64_t stack_top = reinterpret_cast<uint64_t>(stack_buf + sizeof(stack_buf));

    InterruptFrame frame{};
    frame.cs     = 0x33;  // user code segment (RPL=3)
    frame.ss     = 0x2B;
    frame.rsp    = stack_top;
    frame.rip    = 0x400000;
    frame.rflags = 0x202;
    frame.rax    = 0x1234;
    frame.rbx    = 0x5678;

    signal_setup_frame(&frame, Signal::kSigusr1, 0x500000, 0);

    // Frame redirected to enter the handler with the signal number in %rdi.
    TEST_ASSERT_EQ(frame.rip, uint64_t{0x500000});
    TEST_ASSERT_EQ(frame.rdi, static_cast<uint64_t>(Signal::kSigusr1));

    // The handler RSP points at a return-address slot holding the trampoline addr.
    const uint64_t handler_rsp = frame.rsp;
    const uint64_t ret_addr    = *reinterpret_cast<uint64_t*>(handler_rsp);
    const uint8_t* tramp       = reinterpret_cast<const uint8_t*>(ret_addr);
    TEST_ASSERT_EQ(tramp[0], 0xCD);  // int
    TEST_ASSERT_EQ(tramp[1], 0x80);  // $0x80

    // SignalFrame sits just above the return-address slot.
    const SignalFrame* sf = reinterpret_cast<const SignalFrame*>(handler_rsp + 8);
    TEST_ASSERT_EQ(sf->magic, kSigFrameMagic);
    TEST_ASSERT_EQ(sf->sig, static_cast<uint64_t>(Signal::kSigusr1));
    TEST_ASSERT_EQ(sf->rip, uint64_t{0x400000});  // saved interrupted RIP
    TEST_ASSERT_EQ(sf->rsp, stack_top);           // saved user RSP
    TEST_ASSERT_EQ(sf->rax, uint64_t{0x1234});
    TEST_ASSERT_EQ(sf->rbx, uint64_t{0x5678});
}

void test_sigreturn_restores_context() {
    using cinux::arch::InterruptFrame;
    static uint8_t stack_buf[4096];
    const uint64_t stack_top = reinterpret_cast<uint64_t>(stack_buf + sizeof(stack_buf));

    InterruptFrame frame{};
    frame.cs  = 0x33;
    frame.ss  = 0x2B;
    frame.rsp = stack_top;
    frame.rip = 0x400000;
    frame.rax = 0xAA;
    frame.rdx = 0xBB;

    signal_setup_frame(&frame, Signal::kSigusr2, 0x500000, 0);

    // Simulate the handler returning into the int $0x80 trampoline: at that
    // point user RSP = handler RSP + 8, which is exactly the SignalFrame.
    InterruptFrame ret_frame{};
    ret_frame.rsp = frame.rsp + 8;
    sigreturn_handler(&ret_frame);

    TEST_ASSERT_EQ(ret_frame.rip, uint64_t{0x400000});
    TEST_ASSERT_EQ(ret_frame.rsp, stack_top);
    TEST_ASSERT_EQ(ret_frame.rax, uint64_t{0xAA});
    TEST_ASSERT_EQ(ret_frame.rdx, uint64_t{0xBB});
}

}  // namespace test_sig_frame

// ============================================================
// Entry point
// ============================================================

extern "C" void run_signal_tests() {
    TEST_SECTION("Signal Tests (F3-M1-1)");

    RUN_TEST(test_sigset::test_set_add_del_member);
    RUN_TEST(test_sigset::test_make_mask_bit_position);
    RUN_TEST(test_sig_default::test_default_actions);
    RUN_TEST(test_sig_meta::test_uncatchable);
    RUN_TEST(test_sig_meta::test_valid);
    RUN_TEST(test_sig_state::test_sigaction_default_ctor);
    RUN_TEST(test_sig_state::test_task_has_signal_fields);

    RUN_TEST(test_sig_send::test_send_sets_pending);
    RUN_TEST(test_sig_send::test_send_ignored_is_discarded);
    RUN_TEST(test_sig_send::test_send_uncatchable_overrides_ignore);
    RUN_TEST(test_sig_pick::test_pick_clears_and_respects_block);
    RUN_TEST(test_sig_pick::test_pick_skips_custom);
    RUN_TEST(test_sig_syscall::test_sigaction_install_and_query);
    RUN_TEST(test_sig_syscall::test_sigaction_rejects_uncatchable);
    RUN_TEST(test_sig_syscall::test_sigprocmask_block_unblock);
    RUN_TEST(test_sig_syscall::test_sigprocmask_cannot_block_sigkill);
    RUN_TEST(test_sig_syscall::test_kill_self_pends);
    RUN_TEST(test_sig_syscall::test_kill_via_registry);

    RUN_TEST(test_sig_frame::test_setup_frame_redirects_to_handler);
    RUN_TEST(test_sig_frame::test_sigreturn_restores_context);

    TEST_SUMMARY();
}
