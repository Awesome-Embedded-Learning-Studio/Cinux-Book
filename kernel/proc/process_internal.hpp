/**
 * @file kernel/proc/process_internal.hpp
 * @brief Shared internal state for process sub-modules
 *
 * Declares the TID counter, stack-virtual-address allocator, and the CoW
 * page-table copier shared between task_builder.cpp, fork.cpp, and clone.cpp.
 * Not part of the public API.
 */

#pragma once

#include <stdint.h>

#include "kernel/lib/atomic.hpp"
#include "kernel/proc/process.hpp"

namespace cinux::proc {

extern cinux::lib::Atomic<uint64_t> next_tid;

/**
 * @brief Inherit process-group / session membership into a forked/cloned child
 *
 * Called after the child TCB is memcpy'd from the parent (F3-M3 batch 1).
 * A "root" fork (parent->pgid == 0, i.e. the parent is a kernel/bootstrap
 * task with no group) founds a new process group AND session -- the child
 * becomes its own leader (pgid == sid == child_pid).  Otherwise the child
 * inherits the parent's group, session, session-leader pointer, and
 * controlling terminal.  Centralised here so fork() and clone() share one
 * testable rule instead of relying on the implicit memcpy copy.
 *
 * @param child     Child task (already memcpy'd from parent)
 * @param parent    Parent task
 * @param child_pid PID assigned to the child
 */
void inherit_process_identity(Task* child, const Task* parent, int child_pid);

uint64_t alloc_stack_vaddr(uint64_t pages);

/**
 * @brief Recursively copy a page-table level for Copy-On-Write fork/clone
 *
 * Defined in fork.cpp; used by fork() and clone()'s cow_clone_address_space().
 * At the PT (leaf) level shares physical pages and marks writable entries
 * read-only with FLAG_COW; at intermediate levels allocates new table pages
 * and recurses.
 */
void copy_page_table_level(uint64_t src_phys, uint64_t dst_phys, int level);

}  // namespace cinux::proc
