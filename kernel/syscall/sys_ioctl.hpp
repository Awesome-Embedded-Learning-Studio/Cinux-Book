/**
 * @file kernel/syscall/sys_ioctl.hpp
 * @brief sys_ioctl handler declaration (F10-M1 batch 4)
 *
 * Minimal stub.  musl probes ioctl(TIOCGWINSZ) on first stdout write to
 * decide buffering; returning -ENOTTY (not a terminal) makes it fall back
 * to line buffering.  Device-specific ioctls are added as drivers need them.
 */

#pragma once

#include <stdint.h>

namespace cinux::syscall {

/// ioctl(fd, request, ...) -- device control. Currently a -ENOTTY stub.
int64_t sys_ioctl(uint64_t fd, uint64_t request, uint64_t arg, uint64_t, uint64_t, uint64_t);

}  // namespace cinux::syscall
