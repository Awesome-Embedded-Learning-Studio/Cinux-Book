/**
 * @file    sys_mprotect.hpp
 * @brief   mprotect: rewriting the terms of recorded promises.
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
 * @brief         Serves mprotect(addr, len, prot).
 *
 * @param[in]     addr_word   The stretch's first byte; rounded down to
 *                            its page.
 * @param[in]     len_word    How many bytes; rounded up to whole pages.
 * @param[in]     prot_word   The new protection bits.
 * @return        Zero, or a negative errno.
 * @note          What changes here is the register, not the tables: the
 *                promise is rewritten, and the fault path that reads the
 *                promise is what enforces it, once mapping and fault
 *                speak the same terms. A stretch that would split a
 *                recorded span is refused untouched.
 * @warning       None
 * @throws        None
 * @since         0.2.0
 * @ingroup       kernel_syscall
 */
long long HandleMprotect(Word addr_word, Word len_word, Word prot_word);

}  // namespace cinux::syscall
