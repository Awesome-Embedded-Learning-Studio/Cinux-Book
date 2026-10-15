/**
 * @file    task.hpp
 * @brief   The task control block and the pure logic that arms one.
 *
 * A Task is a register image plus identity plus the stack it owns.
 * Everything the switcher needs lives in CpuContext; everything the
 * scheduler needs lives in the three words around it. Arming a fresh
 * task — zeroing the image, laying in the trampoline pair, planting
 * the overflow magic — is pure arithmetic over caller-given addresses,
 * so it lives in its own translation unit that host tests link
 * directly, the heap_runtime split one theme over. Allocation itself
 * (PMM pages, heap object) stays kernel-side in task_runtime.cpp.
 *
 * @author  Charliechen114514
 * @date    2026-10-12
 * @version 0.2
 * @since   0.1.0
 * @ingroup kernel_proc
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "cinux/container/self_list.hpp"
#include "kernel/arch/x86_64/context.hpp"
#include "kernel/fs/file_table.hpp"
#include "kernel/fs/fs_config.hpp"

namespace cinux::mm {
class AddressSpace;
class VmaStore;
}  // namespace cinux::mm

namespace cinux::proc {

/**
 * @brief         Lifecycle of one task.
 * @since         0.1.0
 * @ingroup       kernel_proc
 */
enum class TaskState : unsigned char {
    kReady,    ///< Queued, waiting for a CPU.
    kRunning,  ///< On the CPU right now.
    kBlocked,  ///< Parked on a wait queue, not schedulable until woken.
    kZombie,   ///< Exited with a parent alive; the body stays until reaped.
    kDead,     ///< Entry returned and exited; memory pending reclaim.
};

/**
 * @brief         The kernel-thread control block.
 * @note          Minimal on purpose: fields arrive with the station
 *                that consumes them. stack_base is the virtual
 *                (direct-map) address of the first page, kept because
 *                freeing needs the allocation base, not any pointer
 *                the running thread ever holds. The family words —
 *                parent, child_link, exit_code — exist so a task can
 *                be waited on: an exited child turns zombie instead of
 *                vanishing, and the parent's wait is what retires it.
 *                The roll of offspring itself lives in the scheduler,
 *                one list for the whole machine, because a list type
 *                cannot be a member of the very task it links.
 * @since         0.1.0
 * @ingroup       kernel_proc
 */
struct Task : cinux::base::container::SelfNode<Task> {
    cinux::base::container::SelfNode<Task> child_link;  ///< Seat in the scheduler's offspring roll.
    /// Seat on a sync-family wait list (Mutex, Semaphore, Completion),
    /// kept apart from the base node so parking for a lock while a
    /// parent waits above cannot ride one link into both lists.
    cinux::base::container::SelfNode<Task> sync_wait_link;
    cinux::arch::CpuContext                ctx{};  ///< Register image the switcher moves.
    cinux::arch::FpuImage                  entry_fpu{};
    unsigned long long                     fpu_gate_depth{};
    TaskState                              state{};  ///< Where this task is in its life.
    unsigned long long                     pid{};    ///< Identity, handed out by the scheduler.
    Task*              parent{};      ///< Who may wait for this task; null when nobody can.
    long long          exit_code{};   ///< What a zombie has to hand its parent.
    const char*        name{};        ///< Label for dumps, caller-owned storage.
    unsigned long long stack_base{};  ///< Direct-map base of the owned stack.
    unsigned long long user_root{};   ///< PML4 of the user world, 0 while kernel-only.
    cinux::mm::AddressSpace*
        space{};  ///< The world this task retires with; null while kernel-only or borrowed.
    cinux::mm::VmaStore*
        vmas{};  ///< The registered ranges of its user world; lives exactly with space.
    cinux::fs::FileTable files;               ///< This task's open files, keyed by descriptor.
    char cwd[cinux::fs::kFsPathMax] = {'/'};  ///< Working directory, absolute, NUL-terminated.

    /**
     * @brief         The parent's pid, zero when nobody waits above.
     * @return        Parent pid, or 0 for a parentless task.
     * @since         0.2.0
     * @ingroup       kernel_proc
     */
    [[nodiscard]] unsigned long long ppid() const { return parent != nullptr ? parent->pid : 0; }
};

/**
 * @brief         Everything arming a fresh context needs, as addresses.
 * @since         0.1.0
 * @ingroup       kernel_proc
 */
struct StartPlan {
    unsigned long long trampoline;    ///< First rip of the task.
    unsigned long long entry;         ///< Goes to r12; the trampoline calls it.
    unsigned long long exit_hook;     ///< Goes to r13; the trampoline jumps to it on return.
    unsigned long long stack_bottom;  ///< Lowest stack byte; receives the magic.
    unsigned long long stack_top;     ///< First rsp, 16-byte aligned.
};

/**
 * @brief         Turn a zeroed context plus a plan into a runnable task
 *                image, and plant the overflow magic.
 * @param[out]    ctx    The context to arm; fully overwritten.
 * @param[in]     plan   Addresses describing this task's start.
 * @return        None.
 * @note          Pure arithmetic plus one store through plan's own
 *                stack addresses, so host tests drive it with plain
 *                buffers. The FXSAVE64 image is preloaded with the
 *                clean x87/SSE environment (control word 0x037F,
 *                MXCSR 0x1F80) so the first fxrstor64 lands the task
 *                in a defined floating-point state.
 * @since         0.1.0
 * @ingroup       kernel_proc
 */
void PrepareTaskContext(cinux::arch::CpuContext& ctx, const StartPlan& plan);

/**
 * @brief         Fluent builder for kernel tasks.
 * @note          Entry is required, name defaults. build() allocates
 *                the stack (contiguous pages from the PMM) and the
 *                block (kernel heap), rolling back fully on failure
 *                and returning nullptr.
 * @since         0.1.0
 * @ingroup       kernel_proc
 */
class TaskBuilder {
public:
    /**
     * @brief         Set the thread entry; required before build().
     *
     * @param[in]     entry   The first function the task runs.
     * @return        This builder, for chaining.
     * @since         0.1.0
     * @ingroup       kernel_proc
     */
    TaskBuilder& set_entry(void (*entry)());

    /**
     * @brief         Set the dump label; caller keeps the storage alive.
     *
     * @param[in]     name   Label for dumps; caller-owned storage.
     * @return        This builder, for chaining.
     * @since         0.1.0
     * @ingroup       kernel_proc
     */
    TaskBuilder& set_name(const char* name);

    /**
     * @brief         Adopting parent: the built task waits on this one.
     * @param[in]     parent   The task whose children list takes the
     *                         built task; its pid becomes the child's
     *                         reported parent.
     * @return        This builder, for chaining.
     * @note          Without a parent the task exits straight to
     *                reclaim — nobody can wait for it, so nothing is
     *                held back.
     * @since         0.2.0
     */
    TaskBuilder& set_parent(Task& parent);

    /// Allocate and arm the task, or nullptr with everything returned.
    [[nodiscard]] Task* build();

private:
    void (*entry_)()    = nullptr;
    const char* name_   = "unnamed";
    Task*       parent_ = nullptr;
};

/**
 * @brief         Seat the assembly-backed switch executor into the
 *                scheduler.
 * @return        None.
 * @note          The kernel bring-up call: once this runs, spawn,
 *                yield and exit operate on real register images.
 *                Test worlds install their own sink instead and never
 *                link this file.
 * @since         0.2.0
 * @ingroup       kernel_proc
 */
void InstallKernelSwitchSink();

}  // namespace cinux::proc
