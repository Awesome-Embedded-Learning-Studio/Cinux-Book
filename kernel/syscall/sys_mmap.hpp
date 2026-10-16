/**
 * @file    sys_mmap.hpp
 * @brief   mmap: promising a range of the user half.
 * @author  Charliechen114514
 * @date    2026-10-14
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_syscall
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "kernel/syscall/syscall.hpp"

namespace cinux::syscall {

/**
 * @brief         Serves mmap(hint, len, prot, flags, fd, offset).
 *
 * @param[in]     frame   The trap frame; rsi carries the length, rdx
 *                        the protection, r10 the flags, r8 the
 *                        descriptor, r9 the file offset. The address
 *                        hint in rdi is ignored, the spot comes from
 *                        the register's own hunt.
 * @return        The first byte of the promised range, or a negative
 *                errno.
 * @note          Laziness is the point: the call registers the promise
 *                and hands back an address; no page is mapped, and the
 *                fault that first touches the range is the call that
 *                fills it. Anonymous mappings take any protection; a
 *                file mapping refuses PROT_WRITE at this station —
 *                shared writable files are a later story — and reads
 *                the node out of the caller's descriptor table.
 * @warning       None
 * @throws        None
 * @since         0.2.0
 * @ingroup       kernel_syscall
 */
long long HandleMmap(const SyscallFrame& frame);

}  // namespace cinux::syscall
