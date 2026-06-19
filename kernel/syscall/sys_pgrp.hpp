/**
 * @file kernel/syscall/sys_pgrp.hpp
 * @brief Process-group / session syscall handlers (F3-M3 batch 3)
 *
 * Thin syscall layer over cinux::proc::setpgid/getpgid/getsid/setsid.  A pid
 * argument of 0 always means "the calling task".  Returns the proc layer's
 * value (>= 0 / -errno).
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>

#include "kernel/arch/x86_64/syscall.hpp"

namespace cinux::syscall {

/// setpgid(pid, pgid): pid 0 => caller.
int64_t sys_setpgid(uint64_t pid, uint64_t pgid, uint64_t, uint64_t, uint64_t, uint64_t);

/// getpgid(pid): pid 0 => caller.  Returns the group id.
int64_t sys_getpgid(uint64_t pid, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

/// getsid(pid): pid 0 => caller.  Returns the session id.
int64_t sys_getsid(uint64_t pid, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

/// setsid(): caller founds a new session + group.  Returns the new sid.
int64_t sys_setsid(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

}  // namespace cinux::syscall
