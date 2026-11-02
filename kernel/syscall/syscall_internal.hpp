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
#include "kernel/fs/open_file.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/syscall/syscall.hpp"

namespace cinux::syscall {

/// One argument as the trampoline hands it over: a raw word.
using Word = unsigned long long;

/// What one numbered syscall looks like to the dispatch table: the
/// entry frame itself, carrying the number, all six ABI words and
/// the whole user register image. The face is const — nearly every
/// handler only reads it — and the two that legitimately rewrite the
/// image (exec, sigreturn) take back writability inside their own
/// bodies, so the dispatch table can promise read-only for the rest.
using Handler = long long (*)(const SyscallFrame& frame);

/**
 * @brief         Translates a KernelError into its Linux errno value.
 * @param[in]     error   The error the kernel side produced.
 * @return        The errno ring 3 expects, negated by the caller.
 * @since         0.1.0
 * @ingroup       kernel_syscall
 */
int ErrnoFrom(base::KernelError error);

/**
 * @brief         Tears one span of a task's world down to unmapped.
 * @param[in]     self    The task whose range register and space change.
 * @param[in]     start   First byte of the span, page-aligned.
 * @param[in]     bytes   Length of the span, page-multiple.
 * @return        The range register's own verdict; the page sweep runs
 *                only when it succeeds.
 * @note          The corridor munmap walks and a fixed mmap borrows:
 *                overlapping ranges leave the register, and pages still
 *                standing in the tables come out frame by frame. A task
 *                without a live space keeps the bookkeeping alone.
 * @since         0.4.0
 * @ingroup       kernel_syscall
 */
base::Result<void> ReclaimUserSpan(const proc::Task& self, unsigned long start,
                                   unsigned long bytes);

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
 * @brief         Copies a ring-3 path and lays it on the caller's cwd.
 * @param[in]     source   The user address of the path, relative or
 *                         absolute.
 * @param[out]    out      Kernel storage for the absolute result.
 * @param[in]     cap      How many bytes out holds.
 * @return        The folded path length, or a negative errno
 *                (-kEfault, -kEinval, -kEnametoolong when folding
 *                overflows).
 * @note          The corridor every path syscall walks: dots fold away,
 *                dot-dot pops, and a relative path lands under the
 *                calling task's working directory.
 * @since         0.3.0
 * @ingroup       kernel_syscall
 */
long long FoldUserPath(Word source, char* out, unsigned long cap);

/**
 * @brief         Fold a user path relative to an open directory or AT_FDCWD.
 * @return        Folded length, or a negative errno.
 * @note          Output holds at least fs::kFsPathMax bytes.
 * @since         0.3.0
 * @ingroup       kernel_syscall
 */
long long FoldUserPathAt(Word descriptor, Word address, char* output);

/**
 * @brief         The task currently holding the CPU.
 * @return        The task whose syscall this is.
 * @since         0.1.0
 * @ingroup       kernel_syscall
 */
proc::Task* CurrentTask();

/**
 * @brief Current RLIMIT_NOFILE soft limit, bounded by the nr_open ceiling.
 * @return Exclusive descriptor ceiling for new allocations.
 */
unsigned long FileDescriptorLimit();


/**
 * @brief         Whether a user stretch may be read through.
 * @param[in]     start   First byte of the stretch, as ring 3 offered it.
 * @param[in]     bytes   How many bytes the access touches.
 * @return        true when the stretch is non-empty and lies wholly
 *                inside the user half.
 * @note          The minimal check the station 21 shape can afford:
 *                bounds, not translations. A pointer into a hole still
 *                reaches the fault path; the pair only keeps a handler
 *                from touching kernel words or wrapping the counter.
 * @since         0.5.0
 * @ingroup       kernel_syscall
 */
[[nodiscard]] inline bool UserRangeReadable(Word start, Word bytes) {
    return bytes != 0 && start != 0 && start < kUserAddressLimit &&
           bytes <= kUserAddressLimit - start;
}

/**
 * @brief         Whether a user stretch may be written through.
 * @param[in]     start   First byte of the stretch, as ring 3 offered it.
 * @param[in]     bytes   How many bytes the access touches.
 * @return        true under the same terms as the readable pair.
 * @note          Same bounds, no more: the write-per-page terms arrive
 *                with the fault path's own promotion, not here.
 * @since         0.5.0
 * @ingroup       kernel_syscall
 */
[[nodiscard]] inline bool UserRangeWritable(Word start, Word bytes) {
    return UserRangeReadable(start, bytes);
}

/**
 * @brief         Range-checks a descriptor and hands back its live open.
 * @param[in]     descriptor   The number ring 3 offered.
 * @return        The open, or kInvalidArgument (out of range),
 *                kNotFound (no open file there).
 * @since         0.1.0
 * @ingroup       kernel_syscall
 */
base::Result<base::Shared<fs::OpenFile>> LiveSlot(Word descriptor);

}  // namespace cinux::syscall
