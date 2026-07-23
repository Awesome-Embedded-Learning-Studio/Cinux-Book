/**
 * @file kernel/syscall/sys_exit.hpp
 * @brief sys_exit handler declaration
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>

#include "kernel/arch/x86_64/syscall.hpp"

namespace cinux::syscall {

/**
 * @brief Terminate the current task with an exit code
 *
 * Marks the current task as Dead and yields to the scheduler.
 * Does not return.
 *
 * @param code  Exit code (0 = success)
 * @return Should not return; scheduler picks the next task
 */
int64_t sys_exit(uint64_t code, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

/**
 * @brief Terminate the current thread group with an exit code (F10-M1 batch 4)
 *
 * musl's exit()/QuickExit path calls exit_group first, falling back to
 * exit() only if it returns.  For the current single-threaded musl model
 * exit_group == exit (terminate the lone task); terminating every thread in
 * the group is a follow-up once CLONE_THREAD programs are exercised.
 *
 * @param code  Exit code (0 = success)
 * @return Should not return.
 */
int64_t sys_exit_group(uint64_t code, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

}  // namespace cinux::syscall
