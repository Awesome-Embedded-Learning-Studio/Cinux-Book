/**
 * @file    fork.hpp
 * @brief   Assembly of the child a fork call leaves behind.
 *
 * The one question fork answers is "what does a second copy of me look
 * like", and the answer has three parts this file braids together: a
 * page-table world whose every page is shared copy-on-write with the
 * parent's, a register image that resumes at the syscall return path
 * with rax forced to zero, and the inheritables — descriptors with
 * their offsets, the working directory, the parent's name in the
 * roll. The child comes back unseated: the caller assigns it a pid by
 * seating it, which keeps the scheduler the only place identities are
 * handed out.
 *
 * @author  Charliechen114514
 * @date    2026-10-12
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_proc
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "kernel/proc/task.hpp"

namespace cinux::proc {

/**
 * @brief         Build the unseated child of the running task.
 *
 * @param[in]     frame_address   The syscall entry frame the parent is
 *                                answering from, as the entry assembly
 *                                banked it.
 * @return        The child task, or nullptr with everything rolled back.
 * @note          The child's world shares every parent page
 *                copy-on-write; its context resumes at the syscall
 *                return tail with rax zero, so the child experiences
 *                fork as a call that returns 0. The floating-point
 *                image rides along from the parent's last switch, not
 *                from this instant — the teaching-shape cut the first
 *                act made too. Callers own seating, and with it the
 *                pid.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       kernel_proc
 */
[[nodiscard]] Task* BuildForkChild(unsigned long long frame_address);

}  // namespace cinux::proc
