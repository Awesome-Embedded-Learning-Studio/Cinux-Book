/**
 * @file    procfs_init.hpp
 * @brief   Wiring the facts into procfs: the scheduler, the pages, the
 *          running program.
 *
 * ProcFs itself knows nothing about tasks or memory — it answers from
 * the injected source. This is the kernel-only step below: the living
 * process roll from the scheduler's registry (snapshots under its
 * lock), the memory counters from the page manager, and self/exe from
 * the current task's own program, its bytes delegated back through the
 * file world so the very file a task runs is the file it reads.
 *
 * @author  Charliechen114514
 * @date    2026-10-19
 * @version 0.1
 * @since   0.5.0
 * @ingroup kernel_fs
 * @copyright Copyright (c) 2026
 */

#pragma once

namespace cinux::fs {

class ProcFs;

/**
 * @brief         Wires one procfs instance to the kernel's own facts.
 *
 * @param[in,out] procfs   The instance receiving the source.
 * @return        None
 * @note          Bring-up wiring, single-threaded, before the mount
 *                goes live.
 * @since         0.5.0
 * @ingroup       kernel_fs
 */
void WireKernelProcSource(ProcFs& procfs);

}  // namespace cinux::fs
