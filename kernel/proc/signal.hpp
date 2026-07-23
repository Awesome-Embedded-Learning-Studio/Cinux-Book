/**
 * @file kernel/proc/signal.hpp
 * @brief POSIX signal vocabulary, disposition, and delivery (F3-M1)
 *
 * Defines the core signal numbers (Signal), the 64-bit signal set (SigSet),
 * the per-signal disposition (SigAction), the lookup helpers, the delivery
 * machinery, and the signal-frame layout used to enter/return from custom
 * user handlers.
 *
 *   Batch 1: vocabulary, dispositions, lookup helpers.
 *   Batch 2: pid->Task registry, signal_send, deliverable selection,
 *            default-action execution, syscall-path check-and-deliver.
 *   Batch 3: signal frame + custom-handler delivery on the interrupt return
 *            path, and sigreturn via an int $0x80 trampoline.
 *
 * Namespace: cinux::proc
 */

#pragma once

#include <stdint.h>

namespace cinux::arch {
struct InterruptFrame;  // full definition in idt.hpp
}  // namespace cinux::arch

namespace cinux::proc {

// ============================================================
// Signal numbers (core POSIX subset, 1..22)
// ============================================================

enum class Signal : int {
    kSighup    = 1,   ///< Hangup detected on controlling terminal
    kSigint    = 2,   ///< Interrupt from keyboard (Ctrl+C)
    kSigquit   = 3,   ///< Quit from keyboard (Ctrl+\)
    kSigill    = 4,   ///< Illegal instruction
    kSigtrap   = 5,   ///< Trace/breakpoint trap
    kSigabrt   = 6,   ///< Abort signal from abort(3)
    kSigbus    = 7,   ///< Bus error (bad memory access)
    kSigfpe    = 8,   ///< Floating-point exception
    kSigkill   = 9,   ///< Kill (cannot be caught, blocked, or ignored)
    kSigusr1   = 10,  ///< User-defined signal 1
    kSigsegv   = 11,  ///< Invalid memory reference
    kSigusr2   = 12,  ///< User-defined signal 2
    kSigpipe   = 13,  ///< Broken pipe: write to pipe with no readers
    kSigalrm   = 14,  ///< Timer signal from alarm(2)
    kSigterm   = 15,  ///< Termination signal
    kSigstkflt = 16,  ///< Stack fault on coprocessor
    kSigchld   = 17,  ///< Child stopped or terminated
    kSigcont   = 18,  ///< Continue if stopped
    kSigstop   = 19,  ///< Stop process (cannot be caught, blocked, or ignored)
    kSigtstp   = 20,  ///< Stop typed at terminal (Ctrl+Z)
    kSigttin   = 21,  ///< Terminal input for background process
    kSigtou    = 22,  ///< Terminal output for background process
};

/// Highest supported signal number in the core set.
constexpr int kSignalMax = 22;

/// Array extent for per-signal tables (index 0 is unused; 1..kSignalMax).
constexpr int kSignalCount = kSignalMax + 1;

// ============================================================
// Signal sets (64-bit bitmask, bit n <=> signal n)
// ============================================================

using SigSet = uint64_t;

inline constexpr SigSet sig_make_mask(Signal sig) {
    return SigSet{1} << static_cast<unsigned>(sig);
}

inline void sig_set_add(SigSet& set, Signal sig) {
    set |= sig_make_mask(sig);
}

inline void sig_set_del(SigSet& set, Signal sig) {
    set &= ~sig_make_mask(sig);
}

inline bool sig_is_member(SigSet set, Signal sig) {
    return (set & sig_make_mask(sig)) != 0;
}

/// Validate a raw int as a core signal number (1..kSignalMax).
inline bool signal_valid(int num) {
    return num >= static_cast<int>(Signal::kSighup) && num <= kSignalMax;
}

// ============================================================
// Dispositions
// ============================================================

/// Default disposition when a signal is delivered with no handler installed.
enum class SigDefault : uint8_t {
    kTerminate,  ///< Terminate the process
    kCoreDump,   ///< Terminate and dump core
    kIgnore,     ///< Discard the signal
    kStop,       ///< Stop the process (job control)
    kContinue,   ///< Resume a stopped process
};

/// How a process elects to handle a given signal.
enum class HandlerType : uint8_t {
    kDefault,  ///< SIG_DFL -- apply the default disposition
    kIgnore,   ///< SIG_IGN -- discard the signal
    kCustom,   ///< A user-mode handler is installed
};

/// Per-signal disposition installed via sigaction.
struct SigAction {
    HandlerType type{HandlerType::kDefault};
    uint64_t    handler_addr{0};      ///< User-mode handler (valid when type == kCustom)
    SigSet      sa_mask{0};           ///< Extra signals blocked while the handler runs
    bool        sa_restart{false};    ///< SA_RESTART: restartable syscalls
    bool        sa_resethand{false};  ///< SA_RESETHAND: reset to default after one delivery
};

// ============================================================
// Shared signal disposition table (F3-M2 batch 3)
// ============================================================

/**
 * @brief Reference-counted signal disposition table
 *
 * Threads created with CLONE_SIGHAND share one instance (each acquire() bumps
 * the refcount); fork/execve and clone-without-SIGHAND each get a private
 * copy via create_copy().  Lives on the kernel heap (slab general cache).
 *
 * A Task holds a SharedSigActions*; clone sharing is just pointer + acquire(),
 * which is why the table is heap-allocated and refcounted rather than an
 * inline array.
 */
struct SharedSigActions {
    uint32_t  refcount;
    SigAction actions[kSignalCount];

    /// Allocate a fresh table with all-default dispositions (refcount = 1).
    static SharedSigActions* create() {
        auto* p = new SharedSigActions;
        if (p != nullptr) {
            p->refcount = 1;
        }
        return p;
    }

    /// Allocate a private copy of @p src (refcount = 1).
    static SharedSigActions* create_copy(const SharedSigActions* src) {
        auto* p = new SharedSigActions;
        if (p != nullptr) {
            p->refcount = 1;
            if (src != nullptr) {
                for (int i = 0; i < kSignalCount; ++i) {
                    p->actions[i] = src->actions[i];
                }
            }
        }
        return p;
    }

    /// Increment the reference count (share this table).
    // F4-M5 R3: atomic refcount -- CLONE_SIGHAND threads on different CPUs share
    // one SharedSigActions (F3-M2); non-atomic ++/-- races once APs run threads
    // (F4-M4).  See SharedCwd::acquire/release for the ACQ_REL rationale.
    void acquire() { __atomic_add_fetch(&refcount, 1, __ATOMIC_ACQ_REL); }

    /// Decrement the reference count; free the table when it reaches zero.
    void release() {
        if (__atomic_sub_fetch(&refcount, 1, __ATOMIC_ACQ_REL) == 0) {
            delete this;
        }
    }
};

// ============================================================
// Lookup helpers (defined in signal.cpp)
// ============================================================

/// Default disposition for @p sig.
SigDefault signal_default_action(Signal sig);

/// SIGKILL and SIGSTOP cannot be caught, blocked, or ignored.
bool signal_is_uncatchable(Signal sig);

// ============================================================
// Task registry (pid -> Task* lookup, used by sys_kill)
// ============================================================

struct Task;  // full definition in process.hpp (which includes this header)

/// Add @p task to the signal-visible registry (called from add_task).
void signal_register_task(Task* task);

/// Remove @p task from the registry (called from remove_task / exit_current).
void signal_unregister_task(Task* task);

/// Look up a task by PID, or nullptr if not registered.
Task* signal_find_task_by_pid(int pid);

/// Look up the PID of the @p n-th registered task (0-based), under the registry
/// lock.  On success returns true and writes *@p out_pid; false if @p n is past
/// the last task.  Lets ProcFS readdir answer "the i-th /proc entry" by walking
/// the registry to that index directly -- no whole-registry snapshot materialised
/// on the (bounded) kernel stack.  Pure additive accessor -- does not alter
/// register/unregister/find_by_pid.  (F6-M2)
bool signal_nth_task_pid(uint32_t n, int* out_pid);

// ============================================================
// Delivery
// ============================================================

/// Queue @p sig on @p target's pending set.  Returns 0 / -errno.
int signal_send(Task* target, Signal sig);

/// Send @p sig to every task in process group @p pgid (F3-M3 batch 2).
/// @p pgid == 0 resolves to the caller's own group.  Returns the number of
/// recipients signalled; 0 means no task matched (the syscall layer maps that
/// to -ESRCH).
int killpg(int pgid, Signal sig);

/// Pick the next deliverable signal for @p task, clear its pending bit, and
/// return its number.  When @p allow_custom is false (the syscall return
/// path, which cannot build a signal frame) Custom dispositions are skipped;
/// when true (the interrupt return path) they are returned for delivery.
/// Returns 0 when nothing is deliverable.
int signal_pick_deliverable(Task* task, bool allow_custom = false);

/// Apply the default disposition for @p sig on @p task.  May terminate the
/// task (does not return in that case).
void signal_exec_default(Task* task, Signal sig);

/// Top-level delivery entry for the syscall return path: pick and act on one
/// pending signal for the current task (Default/Ignore only; Custom is left
/// for the interrupt path which can build a frame).
void signal_check_and_deliver();

// ============================================================
// Signal frame & custom-handler delivery (batch 3)
// ============================================================

/// Magic written into a SignalFrame so sigreturn can sanity-check the frame.
constexpr uint64_t kSigFrameMagic = 0x5349474652414D45ULL;  // "SIGFRAME"

/// Saved user context pushed onto the user stack when a custom handler is
/// entered, and consumed by sigreturn to restore that context.  The layout
/// mirrors the general-purpose register portion of InterruptFrame.
struct SignalFrame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rdx, rcx, rbx, rax;
    uint64_t rip;     ///< Interrupted user RIP
    uint64_t rflags;  ///< Interrupted user RFLAGS
    uint64_t rsp;     ///< Interrupted user RSP (before the frame was pushed)
    uint64_t sig;     ///< Signal number being delivered
    uint64_t magic;   ///< kSigFrameMagic
};

/// The 8-byte sigreturn trampoline stub (F9 batch 1): `int $0x80` (CD 80)
/// followed by nops. Written into the fixed USER_SIGRETURN_PAGE by execve so
/// the handler return address lands on executable code off the (NX) user
/// stack; the int $0x80 traps into the sigreturn IDT gate (vector 0x80).
constexpr uint8_t kSigreturnTrampoline[8] = {0xCD, 0x80, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};

/// Build a signal frame on the user stack (saved context + a return address
/// into the fixed sigreturn page) and redirect @p frame to enter @p handler.
void signal_setup_frame(cinux::arch::InterruptFrame* frame, Signal sig, uint64_t handler_addr,
                        SigSet sa_mask);

/// Interrupt-path delivery: pick and act on one pending signal when about to
/// return to user mode.  Extern "C" -- called from the ISR stubs (interrupts.S).
extern "C" void signal_check_deliver_isr(cinux::arch::InterruptFrame* frame);

/// sigreturn: restore the user context saved by signal_setup_frame.  Entered
/// via the int $0x80 trampoline each handler returns to.  Extern "C" --
/// invoked through a dedicated IDT gate (vector 0x80).
extern "C" void sigreturn_handler(cinux::arch::InterruptFrame* frame);

}  // namespace cinux::proc
