/**
 * @file kernel/syscall/sys_ioctl.cpp
 * @brief sys_ioctl handler implementation (F10-M1 batch 4)
 *
 * No device-specific handling yet: every request returns -ENOTTY.  This is
 * enough for musl's stdio, which only needs the TIOCGWINSZ probe to detect
 * non-terminal fds and pick a buffering mode.  A real fd validity check is
 * deferred until a driver actually registers an ioctl handler.
 */

#include "kernel/syscall/sys_ioctl.hpp"

#include <stdint.h>

#include "kernel/errno.hpp"

namespace cinux::syscall {

int64_t sys_ioctl(uint64_t /*fd*/, uint64_t /*request*/, uint64_t /*arg*/, uint64_t, uint64_t,
                  uint64_t) {
    return -cinux::kEnotty;
}

}  // namespace cinux::syscall
