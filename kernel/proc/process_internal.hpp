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

namespace cinux::proc {

extern cinux::lib::Atomic<uint64_t> next_tid;

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
