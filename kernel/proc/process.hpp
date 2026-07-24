/**
 * @file kernel/proc/process.hpp
 * @brief Process control structures for kernel threading
 *
 * Defines the fundamental data types for task management:
 *   - TaskState: lifecycle states of a task
 *   - CpuContext: callee-saved register snapshot for context switching
 *   - Task: the full task control block (TCB)
 *   - TaskBuilder: fluent builder for constructing Task objects
 *
 * TaskBuilder provides a step-by-step configuration interface:
 *   TaskBuilder()
 *       .set_entry(thread_func)
 *       .set_name("thread_a")
 *       .build();
 *
 * The build() call allocates a TCB from the heap and a kernel stack
 * from the PMM, initialises the CpuContext, and writes a stack
 * overflow detection magic.
 *
 * Depends on: PMM (for stack allocation), Heap (for TCB allocation).
 *
 * Namespace: cinux::proc
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <new>

#include "kernel/mm/address_space.hpp"
#include "kernel/mm/slab.hpp"
#include "kernel/proc/cpu_context.hpp"  // CpuContext (split out for the 500-line limit)
#include "kernel/proc/elf_types.hpp"
#include "kernel/proc/execve.hpp"
#include "kernel/proc/pid.hpp"
#include "kernel/proc/shared_cwd.hpp"
#include "kernel/proc/signal.hpp"
#include "kernel/proc/task_builder.hpp"

namespace cinux::fs {
class FDTable;
}  // namespace cinux::fs

namespace cinux::proc {

class SchedulingClass;

// ============================================================
// Task lifecycle states
// ============================================================

enum class TaskState : uint8_t {
    Running,
    Ready,
    Blocked,
    Stopped,  // Stopped by a job-control signal (SIGSTOP/SIGTSTP/...); excluded from scheduling
    Zombie,
    Dead
};

// CpuContext (callee-saved register snapshot, matches context_switch.S) lives
// in cpu_context.hpp -- split out to keep this file under the 500-line limit.

// SharedCwd (reference-counted cwd) lives in shared_cwd.hpp; included below.

// ============================================================
// Task Control Block
// ============================================================

/**
 * @brief Task Control Block (TCB) representing a kernel thread
 *
 * Contains everything the scheduler needs: saved CPU context,
 * lifecycle state, identity, scheduling priority, stack pointers,
 * and an optional address space for future user-mode tasks.
 */
struct Task {
    // F2-M7b: heap Task objects are served by the dedicated task slab cache.
    static void* operator new(size_t) { return cinux::mm::cache_alloc(cinux::mm::g_task_cache); }
    static void* operator new(size_t, std::align_val_t) {
        return cinux::mm::cache_alloc(cinux::mm::g_task_cache);
    }
    // F3-M2 batch 3: release shared refcounted resources (sig_actions / cwd /
    // fd_table) before the slab memory is returned.  Defined out-of-line
    // (task_builder.cpp) because fd_table's release() needs FDTable's full
    // definition (only forward-declared here).
    static void operator delete(void* p) {
        if (p != nullptr) {
            static_cast<Task*>(p)->release_resources();
        }
        cinux::mm::cache_free(cinux::mm::g_task_cache, p);
    }
    static void operator delete(void* p, std::align_val_t) {
        if (p != nullptr) {
            static_cast<Task*>(p)->release_resources();
        }
        cinux::mm::cache_free(cinux::mm::g_task_cache, p);
    }

    /// Drop this task's references to its shared sig_actions / cwd / fd_table.
    void release_resources();

    /** Saved callee-saved registers for context switching. */
    CpuContext ctx;

    /** SMP migration sync (F4-followup): the CPU this task currently runs on,
     *  or -1 when its ctx is saved / it runs on no CPU.  schedule() sets it to
     *  the local cpu_id just before switching TO this task; context_switch.S
     *  stores -1 once the outgoing task's ctx is fully saved.  pick_next()
     *  skips tasks whose on_cpu != -1 (their ctx save is still in flight on the
     *  CPU they just left), so two CPUs never save/restore the same ctx at once
     *  -- closing the migration race.  Plain int + __atomic_* (x86 TSO). */
    int on_cpu;

    /** Current lifecycle state. */
    TaskState state;

    /** Whether this task is currently on its scheduling class's run queue.
     *  Guarded by the run-queue lock.  Makes RoundRobin::enqueue idempotent: a
     *  task already queued is not re-added, so the classic prepare_to_wait lost-
     *  wakeup race (a producer unblocks a Blocked task -> Ready + enqueued, then
     *  schedule() re-enqueues the same Ready prev) cannot leave a duplicate entry
     *  in the queue (F8-M5). */
    bool on_runq{false};

    /** Unique task identifier (monotonically increasing). */
    uint64_t tid;

    /** Scheduling priority (lower = higher priority, for future use). */
    uint64_t priority;
    /** Time-slice quantum remaining (ticks).  per-task (DEBT-007: was a shared
     *  RoundRobin member, which multi-core tick races shrank to slice/ncpus).
     *  Refilled by SchedulingClass::pick_next / task_fork / TaskBuilder::build. */
    int32_t  quantum_remaining;

    /** Base of the kernel stack allocation (for freeing). */
    uint64_t kernel_stack;

    /** Top of the kernel stack (initial rsp). */
    uint64_t kernel_stack_top;

    /**
     * Virtual address of the guard page below the kernel stack.
     * This page is intentionally left unmapped; a fault here
     * indicates a kernel stack overflow.
     * Zero means no guard page (e.g. the boot task).
     */
    uint64_t kernel_stack_guard_page;

    /** Per-process page tables (nullptr for kernel-only threads). */
    cinux::mm::AddressSpace* addr_space;

    // Program break (user heap end). brk is lazy: sys_brk only moves
    // brk_current; the Heap VMA (created by execve) covers [brk_initial,
    // brk_max) and pages are demand-paged on first access.
    uint64_t brk_current{};  ///< Current heap end
    uint64_t brk_initial{};  ///< Heap start (ELF image end, set by execve)
    uint64_t brk_max{};      ///< Heap ceiling (USER_BRK_MAX low image / USER_MMAP_BASE PIE)

    // F3-M1: POSIX signal state.  The block mask (sig_blocked) is inherited
    // across fork(); pending signals are not (cleared in fork()).  F3-M2 batch
    // 3: dispositions live in a refcounted SharedSigActions so CLONE_SIGHAND
    // threads can share them (fork copies, clone may share).
    SharedSigActions* sig_actions{
        nullptr};              ///< Refcounted dispositions (never null for a live task)
    SigSet   sig_pending{0};   ///< Signals pending delivery
    SigSet   sig_blocked{0};   ///< Signals blocked from delivery
    SigSet   sig_forced{0};    ///< Force-delivered (sync faults): bypass block mask + SIG_IGN at
                               ///< delivery (per-task, SMP-safe)
    uint64_t sig_altstack{0};  ///< sigaltstack base (0 = main stack)
    uint64_t sig_altstack_size{0};  ///< sigaltstack size in bytes

    // F3-M2: futex wait state.  Set in FUTEX_WAIT just before blocking, then
    // matched (uaddr + bitset) and cleared by FUTEX_WAKE.  futex_uaddr==0
    // means "this task is not waiting on a futex".
    uint64_t futex_uaddr{0};   ///< uaddr waited on (0 = not waiting)
    uint32_t futex_bitset{0};  ///< bitset mask (FUTEX_*_BITSET; 0xFFFFFFFF for plain)

    /** Per-process file descriptor table (nullptr = use global). */
    cinux::fs::FDTable* fd_table;

    /** Human-readable task name (static storage, not owned). */
    const char* name;

    /** Process ID (assigned by PidAllocator; 0 = uninitialised). */
    int pid;

    /**
     * Thread-group ID (F3-M2 batch 4).  Equals the group leader's pid; getpid()
     * reports tgid so all threads of a process share one identity.  A
     * single-threaded process has tgid == pid.
     */
    int tgid{0};

    /** Pointer to the thread-group leader task (self for a leader). */
    Task* group_leader{nullptr};

    // ---- Process group / session (F3-M3 batch 1) ----
    // Distinct from tgid (thread group): pgid is the job-control group used
    // for signal broadcast (killpg) and terminal foreground groups.  A task
    // with pgid == pid leads its process group; setpgid()/setsid() manage it.
    int pgid{0};  ///< Process-group ID (0 = uninitialised / kernel thread)

    /** Session ID (0 = uninitialised; equals the session leader's pid). */
    int sid{0};

    /** Pointer to the session-leader task (self for a session leader). */
    Task* session_leader{nullptr};

    /** Controlling terminal index (-1 = none; real tty attach deferred to F10). */
    int controlling_tty{-1};

    // F9 batch 9 (M3): process credentials -- real/effective user/group IDs.
    // Default 0 = root. Inherited across fork()/clone() via their memcpy of the
    // whole Task (the post-memcpy override sections do not touch these), so no
    // explicit copy code is needed. setuid-binary support (execve honoring
    // S_ISUID) and the saved-set (suid/sgid) + fsuid/fsgid are Linux advanced
    // semantics deferred to F6 alongside file-permission enforcement.
    uint32_t uid{0};   ///< Real user ID (0 = root)
    uint32_t euid{0};  ///< Effective user ID (0 = root)
    uint32_t gid{0};   ///< Real group ID (0 = root)
    uint32_t egid{0};  ///< Effective group ID (0 = root)

    // F-ECO batch 2: POSIX file-creation mask (sys_umask). Default 0o022.
    // sys_umask returns the previous mask and sets the new one (masked to 0777).
    // create/mkdir do not yet honour it (ext2 is not task-aware); applying it
    // at creation is a follow-up. Inherited across fork() via the whole-Task
    // memcpy, so no explicit copy code is needed.
    uint32_t umask{0022};

    /**
     * CLONE_CHILD_CLEARTID address (F3-M2 batch 4/5).  On thread exit the
     * kernel writes 0 here and futex_wakes any waiter.  0 = not set.
     */
    uint64_t clear_child_tid{0};

    /** CLONE_CHILD_SETTID address (child writes its tid here on startup). */
    uint64_t set_child_tid{0};

    /** Parent process ID (0 for the kernel init task). */
    int ppid;

    /** Exit status code (valid when state == Zombie). */
    int exit_status;

    /** Singly-linked list of child tasks (head pointer). */
    Task* children;

    /** Pointer to the parent task (nullptr for the kernel init task). */
    Task* parent;

    /** Scheduling class this task belongs to. */
    SchedulingClass* sched_class;

    /** Intrusive link for wait-queue linked lists (Mutex / Semaphore). */
    Task* wait_next;

    /** Generic per-task private data (set before first schedule). */
    void* private_data;

    /** FPU/SSE state (512 bytes, 16-byte aligned for fxsave/fxrstor). */
    alignas(16) uint8_t fpu_state[512];

    // F-ECO batch 8: supplementary groups (getgroups/setgroups -- `id`/`newgrp`).
    // Placed AFTER fpu_state so the fpu_state offset (pinned by the static_assert
    // below for the syscall/interrupt FXSAVE path) is unchanged.  A fixed-size
    // table (freestanding: no <vector>); inherited across fork()/clone() via the
    // whole-Task memcpy (same as uid/gid/umask).  32 is the classic NGROUPS_MAX.
    static constexpr uint32_t kNGroupsMax = 32;       ///< NGROUPS_MAX
    uint32_t                  groups[kNGroupsMax]{};  ///< supplementary group IDs
    uint32_t                  ngroups{0};             ///< number of valid entries

    /** Per-process current working directory (refcounted; F3-M2 batch 3). */
    SharedCwd* cwd{nullptr};

    /** Intrusive link for the global pid->Task registry (sys_kill lookup). */
    Task* registry_next{nullptr};

    /** F-QA Q4e-3 (DEBT-002): link for the deferred-free list. A task that
     *  exits via exit_current() (kernel-thread return / panic / signal kill)
     *  cannot free its own kernel stack (it runs on it); it is enqueued here
     *  and freed by the next task's schedule() entry (reap_deferred). */
    Task* deferred_next{nullptr};

    /** B3b (busybox init): true while this task is blocked in rt_sigtimedwait
     *  waiting for a signal in its wait set.  signal_send() checks it to wake
     *  the sleeper the moment a matching signal lands -- a precise, opt-in wake
     *  (futex/waitpid Blocked waits stay non-interruptible until the broader
     *  "interruptible sleep" TODO at signal_send lands).  After fpu_state. */
    bool sigwait_blocked{false};

    /** Parent blocked by CLONE_VFORK until this task execs or exits. */
    Task* vfork_parent{nullptr};

    // ---- EINTR / interruptible sleep (signal breaks blocking IO) ----
    // A task parked in a blocking syscall (pipe R/W, socket recv/accept, poll)
    // records its wait-queue HEAD address here so signal_send() can wake it for
    // EINTR delivery.  Mutex/Semaphore (kernel-internal) and futex/waitpid
    // sleeps leave this nullptr -- they are NOT interruptible by signals
    // (matches Linux: kernel mutexes / TASK_UNINTERRUPTIBLE never take signals).
    //   wait_enqueue(head,self) -> self->wait_queue_head = &head
    //   signal_send(self,sig)   -> if non-null + deliverable, unblock(self); the
    //                              woken loop checks signal_deliverable_pending()
    //                              and returns -EINTR after unlinking itself
    //                              (under its own lock -- no cross-CPU remove).
    Task** wait_queue_head{nullptr};

    // ITIMER_REAL (setitimer): wall-clock timer ticked from the PIT IRQ (100 Hz);
    // expiry -> SIGALRM + reload from interval (0 = one-shot).  value==0 disarms.
    // Aligned 64-bit rw atomic (TSO); cross-CPU race vs setitimer is benign.
    uint64_t itimer_real_value_ns{0};
    uint64_t itimer_real_interval_ns{0};
};

/// True if @p task has a deliverable pending signal (unblocked or forced) --
/// the EINTR check a blocking syscall loop makes after waking.  signal.cpp.
bool signal_deliverable_pending(const Task* task);

// F4-followup (SMP migration race): context_switch.S writes from->on_cpu = -1
// via a hardcoded offset, relying on rdi (=&from->ctx) being &from because ctx
// is the first data member.  Pin both layout facts the asm depends on.
static_assert(offsetof(Task, ctx) == 0, "ctx at Task+0 (context_switch.S rdi is Task*)");
static_assert(offsetof(Task, on_cpu) == sizeof(CpuContext), "on_cpu offset for context_switch.S");
static_assert(offsetof(Task, fpu_state) == 384, "fpu_state offset for syscall/interrupt FXSAVE");

// ============================================================
// Fork
// ============================================================

/**
 * @brief Fork the current task (Copy-On-Write semantics)
 *
 * Creates a near-identical copy of the calling task:
 *   - Allocates a new PID from the global PidAllocator
 *   - Copies the TCB (pid, ppid, state, etc.)
 *   - If the parent has an AddressSpace, creates a CoW copy of
 *     the user-space page tables (all writable PTEs are marked
 *     read-only with FLAG_COW set)
 *   - Links the child into the parent's children list
 *   - Adds the child task to the scheduler's run queue
 *
 * Return value semantics (set in the child's TCB via ctx.rax):
 *   - Parent: returns child PID (> 0)
 *   - Child:  returns 0
 *
 * @param pid_alloc  Reference to the global PID allocator
 * @return Child PID on success (parent perspective), or -1 on failure
 */
int fork(PidAllocator& pid_alloc);

// ============================================================
// Clone (F3-M2 batch 4)
// ============================================================

/**
 * @brief Create a new task sharing resources per Linux clone() flags
 *
 * Linux syscall 56: `clone(flags, stack, parent_tid, child_tid, tls)`.
 * Unlike fork (full CoW copy), clone selectively SHARES resources:
 *   CLONE_VM      -> share address space (threads)
 *   CLONE_FILES   -> share fd table
 *   CLONE_SIGHAND -> share signal dispositions (implies CLONE_VM)
 *   CLONE_FS      -> share cwd
 *   CLONE_THREAD  -> same thread group (tgid); sibling, not child
 *   CLONE_SETTLS  -> set the child's FS base (TLS) to @p tls
 *   CLONE_PARENT_SETTID / CLONE_CHILD_SETTID -> write the new tid
 *   CLONE_CHILD_CLEARTID -> zero @p child_tid + futex_wake on exit
 *
 * The child returns to user space at the parent's syscall-return RIP with
 * RAX=0 and (if @p stack != 0) RSP=@p stack -- achieved by copying the
 * parent's kernel stack (whose syscall pt_regs frame sits at the top) and
 * patching the child's user-RSP slot.
 *
 * @return child tid (>0) to the parent, or -errno on failure.
 */
int clone(uint64_t flags, uint64_t stack, uint64_t parent_tid, uint64_t child_tid, uint64_t tls);

/**
 * @brief CLONE_CHILD_CLEARTID exit hook (F3-M2 batch 5)
 *
 * If @p task has a clear_child_tid set (CLONE_CHILD_CLEARTID), write 0 to that
 * user address and futex_wake one waiter -- the pthread_join protocol.  Called
 * from the exit path.  No-op when clear_child_tid == 0.
 */
void task_exit_cleartid(Task* task);

// ============================================================
// CoW page fault handling
// ============================================================

/**
 * @brief Attempt to resolve a page fault as a Copy-On-Write trigger
 *
 * Called from the page fault handler when a write to a read-only page
 * is detected.  If the PTE has the FLAG_COW bit set, this function:
 *   1. Allocates a new physical page
 *   2. Copies the contents from the shared page to the new page
 *   3. Updates the PTE to point to the new page with write permission
 *   4. Clears FLAG_COW from the PTE
 *
 * @param fault_vaddr  Virtual address that triggered the fault
 * @return true if the fault was handled as CoW, false otherwise
 */
bool handle_cow_fault(uint64_t fault_vaddr);

// Execve (errno_values, ExecveResult, execve decl) lives in kernel/proc/execve.hpp;
// included below.

// ============================================================
// Waitpid
// ============================================================

/**
 * @brief Result codes from waitpid()
 *
 * Values follow Linux errno conventions so that sys_waitpid can
 * return the negated value directly.
 */
enum class WaitpidResult : int {
    Ok         = 0,    ///< Successfully reaped a child
    NoChildren = -10,  ///< Caller has no children (ECHILD)
    NotFound   = -3,   ///< Specified PID is not a child (ESRCH)
    InvalidPid = -22,  ///< PID argument is invalid (EINVAL)
    NotExited  = -1,   ///< Child exists but has not exited yet
};

/// waitpid() option: return immediately (NotExited) if no child has exited,
/// instead of blocking.  Matches Linux WNOHANG.
constexpr int kWaitNoHang = 1;

/**
 * @brief Wait for a child process to change state
 *
 * If pid > 0, waits for the specific child with that PID.
 * If pid == -1, waits for any child.
 *
 * When the target child is in Zombie state, this function:
 *   1. Collects the child's exit_status into *status
 *   2. Unlinks the child from the parent's children list
 *   3. Frees the child's PID via the PidAllocator
 *   4. Marks the child TCB as Dead
 *
 * If the child has not yet exited (state != Zombie), returns
 * WaitpidResult::NotExited.
 *
 * @param pid        PID of the child to wait for, or -1 for any child
 * @param status     Pointer to store the child's exit status (may be nullptr)
 * @param options    Bitmask: kWaitNoHang => return NotExited instead of blocking
 * @param pid_alloc  Reference to the global PID allocator
 * @param reaped_pid Optional out parameter receiving the child PID that was
 *                   reaped when the result is Ok.
 * @return WaitpidResult::Ok on success, or an error code
 */
WaitpidResult waitpid(int pid, int* status, int options, PidAllocator& pid_alloc,
                      int* reaped_pid = nullptr);

// ============================================================
// Assembly entry point (C linkage)
// ============================================================

/**
 * @brief Low-level context switch primitive
 *
 * Saves callee-saved registers of the current task into `from`,
 * restores callee-saved registers from `to`, and jumps to the
 * saved rip of `to`.
 *
 * @param from  Pointer to the outgoing task's CpuContext
 * @param to    Pointer to the incoming task's CpuContext
 */
extern "C" void context_switch(CpuContext* from, CpuContext* to);

/**
 * @brief Fork child trampoline (assembly, sets rax=0 and returns)
 *
 * Called by the scheduler when the child task is first switched in.
 * Sets rax to 0 so that the child sees fork() return 0, then
 * returns to the fork() call site on the child's stack.  Used by the
 * kernel-fork path (prepare_kernel_fork_context) where the child must
 * resume a kernel caller mid-execution.
 */
extern "C" void fork_child_trampoline();

/**
 * @brief User fork/clone child first-run trampoline (assembly)
 *
 * The child's ctx.rip is set to this by prepare_user_fork_context();
 * ctx.rsp points at a 128-byte pt_regs frame (the parent's syscall frame)
 * copied to the top of the child's clean kernel stack.  ret_from_fork
 * restores the user register state from that frame and SYSRETQs to Ring 3
 * with rax=0 -- the Linux ret_from_fork style.  The child never runs on a
 * copy of the parent's kernel stack, avoiding the gcc-13 -O2+ubsan
 * frame-layout fragility of the old copied-stack RBP-chain approach.
 */
extern "C" void ret_from_fork();

}  // namespace cinux::proc
