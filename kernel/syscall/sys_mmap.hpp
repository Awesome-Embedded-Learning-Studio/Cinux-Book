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

#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::syscall {

/**
 * @brief         Serves mmap(hint, len, prot, flags, fd, offset).
 *
 * @param[in]     first   Unused; the address hint is ignored, the spot
 *                        comes from the register's own hunt.
 * @param[in]     second  Unused; the arguments ride the frame (six of
 *                        them, more than the three-word door carries).
 * @param[in]     third   Unused.
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
long long HandleMmap(Word first, Word second, Word third);

}  // namespace cinux::syscall
