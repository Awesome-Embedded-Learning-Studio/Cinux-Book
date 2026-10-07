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
 * @date    2026-10-07
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_proc
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "cinux/container/self_list.hpp"
#include "kernel/arch/x86_64/context.hpp"

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
    kDead,     ///< Entry returned and exited; memory pending reclaim.
};

/**
 * @brief         The kernel-thread control block.
 * @note          Minimal on purpose: fields arrive with the station
 *                that consumes them. stack_base is the virtual
 *                (direct-map) address of the first page, kept because
 *                freeing needs the allocation base, not any pointer
 *                the running thread ever holds.
 * @since         0.1.0
 * @ingroup       kernel_proc
 */
struct Task : cinux::base::container::SelfNode<Task> {
    cinux::arch::CpuContext ctx{};         ///< Register image the switcher moves.
    TaskState               state{};       ///< Where this task is in its life.
    unsigned long long      tid{};         ///< Identity, handed out by the scheduler.
    const char*             name{};        ///< Label for dumps, caller-owned storage.
    unsigned long long      stack_base{};  ///< Direct-map base of the owned stack.
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
    /// Set the thread entry; required before build().
    TaskBuilder& set_entry(void (*entry)());

    /// Set the dump label; caller keeps the storage alive.
    TaskBuilder& set_name(const char* name);

    /// Allocate and arm the task, or nullptr with everything returned.
    [[nodiscard]] Task* build();

private:
    void (*entry_)()  = nullptr;
    const char* name_ = "unnamed";
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
