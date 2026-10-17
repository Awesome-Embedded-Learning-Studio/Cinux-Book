/**
 * @file    clone.hpp
 * @brief   Assembly of the child a clone call leaves behind.
 *
 * Clone is fork with a say in what stays shared. The base child is
 * built exactly as fork builds it — a copy-on-write world, a fresh
 * table over adopted opens, a frame copy on a fresh kernel stack —
 * and then the flags overlay replaces whole limbs with references to
 * the parent's: CLONE_VM points the child at the maker's address
 * space, CLONE_FILES at the maker's descriptor table, CLONE_SETTLS
 * arms the thread pointer before the first instruction of ring 3 ever
 * runs. One more override belongs to the caller, not the builder: the
 * user stack rides in the plan because the frame copy still names the
 * parent's stack, and a thread that woke there would trample its
 * maker.
 *
 * @author  Charliechen114514
 * @date    2026-10-17
 * @version 0.1
 * @since   0.8.0
 * @ingroup  kernel_proc
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "kernel/proc/task.hpp"

namespace cinux::proc {

/**
 * @brief   What a clone call asks for, after the door has vetted it.
 * @since   0.8.0
 * @ingroup kernel_proc
 */
struct ClonePlan {
    unsigned long long flags;       ///< The served share-or-copy bits.
    unsigned long long user_stack;  ///< First ring 3 stack pointer of the child.
    unsigned long long tls;         ///< Thread pointer, armed as the FS base.
};

/**
 * @brief         Build the unseated child of a clone call.
 *
 * @param[in]     frame_address   The syscall entry frame the parent is
 *                                answering from, as the entry assembly
 *                                banked it.
 * @param[in]     plan            The share-or-copy bits, the child's
 *                                user stack, and the thread pointer.
 * @return        The child task, or nullptr with everything rolled
 *                back.
 * @note          Like the fork child, this one resumes at the syscall
 *                return tail with rax zero and must be seated by the
 *                caller to receive a pid. Unshared limbs are identical
 *                to fork's; shared limbs hold one more owner count, so
 *                the maker may die first and the world survives until
 *                the last member of the group leaves.
 * @warning       None
 * @throws        None
 * @since         0.8.0
 * @ingroup       kernel_proc
 */
[[nodiscard]] Task* BuildCloneChild(unsigned long long frame_address, const ClonePlan& plan);

}  // namespace cinux::proc
