/**
 * @file kernel/syscall/sys_umask.hpp
 * @brief sys_umask handler declaration (F-ECO batch 2)
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>

#include "kernel/arch/x86_64/syscall.hpp"

namespace cinux::syscall {

/// Linux umask(2): set the file-creation mask to (@p mask & 0777) and return the
/// previous mask. The mask is per-task (Task::umask); open/openat(O_CREAT)
/// honours it when applying the requested create mode.
int64_t sys_umask(uint64_t mask, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

}  // namespace cinux::syscall
