/**
 * @file kernel/proc/task_builder.hpp
 * @brief Fluent builder for Task construction (split from process.hpp)
 *
 * Split out so process.hpp stays under the 500-line limit.  TaskBuilder only
 * holds/returns a forward-declared Task pointer (build() returns Task*), so
 * this header does NOT include process.hpp -- no circular dependency.
 *
 * Namespace: cinux::proc
 */

#pragma once

#include <cstdint>

namespace cinux::mm {
class AddressSpace;
}  // namespace cinux::mm

namespace cinux::proc {

struct Task;
class SchedulingClass;

/**
 * @brief Fluent builder for constructing kernel Task objects
 *
 * Accumulates configuration via setter methods, then performs
 * allocation and initialisation in build().  Example usage:
 *
 *   auto* task = TaskBuilder()
 *       .set_entry(my_thread)
 *       .set_name("worker")
 *       .set_priority(1)
 *       .build();
 *
 * At minimum, set_entry() must be called before build().
 */
class TaskBuilder {
public:
    TaskBuilder() = default;

    /** Set the thread entry point.  Required before build(). */
    TaskBuilder& set_entry(void (*entry)());

    /** Set the human-readable task name.  Defaults to "unnamed". */
    TaskBuilder& set_name(const char* name);

    /** Set the scheduling priority.  Defaults to 0. */
    TaskBuilder& set_priority(uint64_t priority);

    /** Set the address space.  Defaults to nullptr (kernel-only). */
    TaskBuilder& set_addr_space(cinux::mm::AddressSpace* space);

    /** Set the scheduling class.  Defaults to nullptr. */
    TaskBuilder& set_sched_class(SchedulingClass* sched_class);

    /**
     * @brief Allocate and initialise the Task
     *
     * Allocates a Task struct from the kernel heap and a kernel
     * stack from the PMM.  Initialises CpuContext so that the
     * first context_switch jumps to the entry point.  Writes a
     * magic value at the stack bottom for overflow detection.
     *
     * @return Pointer to the fully initialised Task, or nullptr on failure
     */
    Task* build();

    /** Magic value written at the bottom of every kernel stack. */
    static constexpr uint64_t STACK_MAGIC = 0xDEADC0DE;

    /** Number of 4 KB pages per kernel stack (8 KB total, Linux-aligned). */
    static constexpr uint64_t STACK_PAGES = 2;
    static_assert(STACK_PAGES == 2,
                  "kernel stacks must stay 8KB; fix stack depth instead of growing them");

private:
    void (*entry_)()                      = nullptr;
    const char*              name_        = "unnamed";
    uint64_t                 priority_    = 0;
    cinux::mm::AddressSpace* addr_space_  = nullptr;
    SchedulingClass*         sched_class_ = nullptr;
};

}  // namespace cinux::proc
