/**
 * @file kernel/syscall/sys_lseek.hpp
 * @brief sys_lseek handler declaration (F10-M1 batch 4)
 *
 * Repositions a file's read/write offset.  musl's fseek/ftell on a FILE*
 * flush and then lseek the underlying descriptor.
 */

#pragma once

#include <stdint.h>

namespace cinux::syscall {

/// lseek(fd, offset, whence) -- reposition fd offset; returns new offset.
int64_t sys_lseek(uint64_t fd, uint64_t offset, uint64_t whence, uint64_t, uint64_t, uint64_t);

}  // namespace cinux::syscall
