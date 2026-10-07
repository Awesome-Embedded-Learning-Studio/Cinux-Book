/**
 * @file    syscall_internal.hpp
 * @brief   Declarations shared by the one-file-per-syscall handlers.
 *
 * Each syscall lives in its own translation unit — sys_read.cpp,
 * sys_open.cpp, and so on — holding its handler plus whatever private
 * help it wants, and its declaration lives in its own small header —
 * sys_read.hpp beside sys_read.cpp. What stays here is only what
 * crosses files: the Word alias, the Handler shape, and the helpers
 * every handler leans on. Nothing here is ring 3's business; the
 * public face stays in syscall.hpp.
 *
 * @author  Charliechen114514
 * @date    2026-10-07
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_syscall
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "cinux/result.hpp"
#include "kernel/fs/file_table.hpp"
#include "kernel/proc/task.hpp"

namespace cinux::syscall {

/// One argument as the trampoline hands it over: a raw word.
using Word = unsigned long long;

/// What one numbered syscall looks like to the dispatch table.
using Handler = long long (*)(Word, Word, Word);

/**
 * @brief         Translates a KernelError into its Linux errno value.
 * @param[in]     error   The error the kernel side produced.
 * @return        The errno ring 3 expects, negated by the caller.
 * @since         0.1.0
 * @ingroup       kernel_syscall
 */
int ErrnoFrom(base::KernelError error);

/**
 * @brief         Copies a path from ring 3 into kernel storage.
 * @param[in]     source   The user address of the path.
 * @param[out]    out      Kernel storage, at least cap bytes.
 * @param[in]     cap      How many bytes out holds.
 * @return        The path length, or a negative errno (-kEfault,
 *                -kEinval for empty or over-long).
 * @since         0.1.0
 * @ingroup       kernel_syscall
 */
long long CopyUserPath(Word source, char* out, unsigned long cap);

/**
 * @brief         The task currently holding the CPU.
 * @return        The task whose syscall this is.
 * @since         0.1.0
 * @ingroup       kernel_syscall
 */
proc::Task* CurrentTask();

/**
 * @brief         Range-checks a descriptor and hands back its live slot.
 * @param[in]     descriptor   The number ring 3 offered.
 * @return        The slot, or kInvalidArgument (out of range),
 *                kNotFound (no open file there).
 * @since         0.1.0
 * @ingroup       kernel_syscall
 */
base::Result<fs::FileSlot*> LiveSlot(Word descriptor);

}  // namespace cinux::syscall
