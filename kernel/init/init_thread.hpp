/**
 * @file    init_thread.hpp
 * @brief   The init thread: mount the world, hatch the first user.
 *
 * PID 1 in the borrowed Linux vocabulary: one task the boot flow hands
 * the machine to, which mounts the root volume and launches the shell,
 * then ends. The model is the shape only — no fork, no signals, no
 * wait; those arrive with the stations that need them. Boot keeps the
 * CPU (PID 0 by the same borrowing) and parks in its idle loop.
 *
 * @author  Charliechen114514
 * @date    2026-10-11
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_init
 * @copyright Copyright (c) 2026
 */

#pragma once

namespace cinux::init {

/**
 * @brief         Builds and seats the init thread.
 *
 * @return        None
 * @note          Call exactly once, after the scheduler's switch sink is
 *                installed: the thread sleeps on the block queue for its
 *                very first mount, so it needs the full machinery alive.
 *                Building it is a loud failure; a machine that cannot
 *                afford one task has nothing to hand off to.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup kernel_init
 */
void StartInitThread();

}  // namespace cinux::init
