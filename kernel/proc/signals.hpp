/**
 * @file    signals.hpp
 * @brief   The per-task signal state and the two doors it opens.
 *
 * Each task carries its own action table, its blocked set and its
 * pending set. Sending marks the pending set; delivering happens on
 * the syscall return path, where the frame is in hand to rewrite: a
 * custom handler is reached by building a small context on the user
 * stack and pointing the frame at it, and sigreturn is the mirror
 * that reads the context back. Default dispositions act here too —
 * termination takes the ordinary exit path, and the stop-and-continue
 * pair is what the scheduler's stopped state hangs off.
 *
 * @author  Charliechen114514
 * @date    2026-10-16
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_proc
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "cinux/spin_mutex.hpp"
#include "kernel/syscall/signal.hpp"
#include "kernel/syscall/syscall.hpp"

namespace cinux::mm {
class AddressSpace;
class VmaStore;
}  // namespace cinux::mm

namespace cinux::proc {

struct Task;

/**
 * @brief         What the default action for one signal does.
 * @since         0.1.0
 * @ingroup       kernel_proc
 */
enum class SignalDisposition : unsigned char {
    kTerminate,  ///< The task exits carrying the signal number.
    kIgnore,     ///< Nothing happens; the signal evaporates.
    kContinue,   ///< A stopped task resumes; anything else ignores it.
    kStop,       ///< The task parks in the stopped state until continued.
};

/**
 * @brief         One task's whole signal world.
 * @note          Zeroed is a valid state: every action default, nothing
 *                blocked, nothing pending — which is exactly what a
 *                fresh task should carry. fork copies the table and the
 *                blocked set but never the pending set; exec resets the
 *                whole thing.
 * @since         0.1.0
 * @ingroup       kernel_proc
 */
struct SignalState {
    cinux::syscall::SigAction actions[cinux::syscall::kSignalCount] =
        {};                          ///< Per-signal disposition.
    cinux::syscall::SigSet blocked;  ///< Held back until unblocked.
    cinux::syscall::SigSet pending;  ///< Sent, awaiting a delivery window.
    cinux::base::SpinMutex gate;     ///< Serializes every pending rewrite;
                                     ///< readers may peek unlocked.

    /// Returns the dispositions, the mask and the pending set to their
    /// boot answers while the gate itself goes on living; exec resets
    /// the signal face whole, and the lock is not the kind of state a
    /// reset should touch.
    void reset();
};

/**
 * @brief         The image sigreturn restores, laid on the user stack.
 * @note          The sixteen frame words ride in SyscallFrame order so
 *                the copy is a straight field-for-field mirror; the
 *                blocked set travels with them because delivery widened
 *                it and only the returning handler knows the old value.
 *                Sized to a multiple of sixteen so the slot above it
 *                stays aligned.
 * @since         0.1.0
 * @ingroup       kernel_proc
 */
struct SigReturnContext {
    unsigned long long     user_rsp{};   ///< The interrupted stack pointer.
    unsigned long long     rcx{};        ///< The interrupted return address.
    unsigned long long     r11{};        ///< The interrupted flags.
    unsigned long long     rax{};        ///< The interrupted accumulator.
    unsigned long long     rdi{};        ///< First argument register.
    unsigned long long     rsi{};        ///< Second argument register.
    unsigned long long     rdx{};        ///< Third argument register.
    unsigned long long     r10{};        ///< Fourth argument register.
    unsigned long long     r8{};         ///< Fifth argument register.
    unsigned long long     r9{};         ///< Sixth argument register.
    unsigned long long     rbx{};        ///< Callee-saved.
    unsigned long long     rbp{};        ///< Callee-saved.
    unsigned long long     r12{};        ///< Callee-saved.
    unsigned long long     r13{};        ///< Callee-saved.
    unsigned long long     r14{};        ///< Callee-saved.
    unsigned long long     r15{};        ///< Callee-saved.
    cinux::syscall::SigSet old_blocked;  ///< The mask before delivery widened it.
    unsigned long long     reserved{};   ///< Padding to a sixteen-byte multiple.
};

static_assert(sizeof(SigReturnContext) == 144, "the context is eighteen quadwords");

/**
 * @brief         Whether a handler may be installed for this signal.
 * @param[in]     number   The signal offered.
 * @return        true for 1..kSignalCount except the uncatchable pair.
 * @note          KILL and STOP stay out of user control, or nothing
 *                could ever stop a runaway task again.
 * @since         0.1.0
 * @ingroup       kernel_proc
 */
[[nodiscard]] bool SignalCatchable(int number);

/**
 * @brief         Whether a mask may hold this signal back.
 * @param[in]     number   The signal offered.
 * @return        true for 1..kSignalCount except the unblockable pair.
 * @since         0.1.0
 * @ingroup       kernel_proc
 */
[[nodiscard]] bool SignalBlockable(int number);

/**
 * @brief         What the kernel does with an unhandled signal.
 * @param[in]     number   The signal arriving.
 * @return        The disposition class the number carries.
 * @since         0.1.0
 * @ingroup       kernel_proc
 */
[[nodiscard]] SignalDisposition DefaultDisposition(int number);

/**
 * @brief         Mark a signal pending on one task.
 * @param[in,out] target   The task to signal.
 * @param[in]     number   The signal, 1..kSignalCount.
 * @return        true when the signal was accepted (including when it
 *                was dropped as ignored); false on a bad number.
 * @note          An ignored signal never lands: the action table says
 *                so at send time. Anything else joins the pending set
 *                and waits for the target's next syscall return. The
 *                stopped state's story is told here, not at delivery:
 *                a continue sweeps the pending stop-class bits and
 *                lifts the halt, and so does a kill — a stopped task
 *                is never scheduled, so it could never deliver its own
 *                death. A stop aimed at a task still queued pulls it
 *                out of its ready queue on the spot; aimed at the
 *                running task it rides as pending, for the delivery
 *                path to act on.
 * @since         0.1.0
 * @ingroup       kernel_proc
 */
bool SendSignal(Task& target, int number);

/**
 * @brief         Sends one signal to every task in a group.
 *
 * @param[in]     pgid     The group's id.
 * @param[in]     number   The signal to deliver.
 * @return        None
 * @note          The walk covers the whole task roll; members gone
 *                before their turn simply miss the news. Terminals
 *                use this for what their foreground group is owed.
 * @since         0.5.0
 * @ingroup       kernel_proc
 */
void SendToGroup(unsigned long long pgid, int number);

/**
 * @brief         Deliver what is pending, on the syscall return path.
 * @param[in,out] frame   The frame the task is about to return on.
 * @return        true when the frame was rewritten for a custom
 *                handler (the caller must hand ring 3 frame's rax,
 *                not its own return value).
 * @note          Consumes ignored signals and runs default actions as
 *                it walks; a custom handler stops the walk, because
 *                one delivery per return is the shape ring 3 expects.
 *                Termination never returns here: it exits through the
 *                ordinary exit path, signal number as the exit word.
 * @since         0.1.0
 * @ingroup       kernel_proc
 */
bool DeliverPendingSignals(cinux::syscall::SyscallFrame& frame);

/**
 * @brief         Map the kernel-owned sigreturn trampoline into a world.
 * @param[in]     space   The address space to map into.
 * @param[in,out] vmas    The range register to record it in.
 * @return        true when the page is mapped and registered.
 * @note          One page above the mmap area, user-readable and
 *                executable but never writable, holding the two
 *                instructions that turn a handler's ret into the
 *                sigreturn syscall: load the number, enter the kernel.
 *                Every world a task may receive a custom handler in
 *                maps it at build time, so fork carries it through
 *                the shared-space clone for free.
 * @since         0.1.0
 * @ingroup       kernel_proc
 */
[[nodiscard]] bool MapSignalTrampoline(const mm::AddressSpace& space, mm::VmaStore& vmas);

/**
 * @brief         The user address the trampoline lives at in every world.
 * @return        The page-aligned trampoline address.
 * @note          One fixed address for every task, above the mmap
 *                area's high-water mark so the allocator never hands
 *                it out; ring 3 does not need to know it, the ret
 *                slot delivery plants does.
 * @since         0.1.0
 * @ingroup       kernel_proc
 */
[[nodiscard]] unsigned long SignalTrampolineAddress();

}  // namespace cinux::proc
