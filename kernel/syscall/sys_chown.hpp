/**
 * @file kernel/syscall/sys_chown.hpp
 * @brief sys_chown handler declaration (F-ECO batch 2)
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>

#include "kernel/arch/x86_64/syscall.hpp"

namespace cinux::syscall {

/// Change the owner of the file at @p resolved_path. A @p uid or @p gid of
/// 0xFFFFFFFF ((uint32_t)-1) leaves that field unchanged, per Linux chown(2).
/// Returns 0 or -errno.
int64_t do_chown_kernel(const char* resolved_path, uint32_t uid, uint32_t gid);

/// Linux chown(2): change owner of @p path_virt.
int64_t sys_chown(uint64_t path_virt, uint64_t uid, uint64_t gid, uint64_t, uint64_t, uint64_t);

}  // namespace cinux::syscall
