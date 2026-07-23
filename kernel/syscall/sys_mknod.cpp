/**
 * @file kernel/syscall/sys_mknod.cpp
 * @brief sys_mknod handler implementation (F8-M2)
 *
 * Creates a named FIFO (S_IFIFO) by registering the leaf name in the
 * FifoRegistry; DevFS's dynamic lookup resolves the name to the FIFO inode on
 * open().  Other node types return -ENOSYS this milestone.  mkfifo(path, mode)
 * is the libc spelling of mknod(path, S_IFIFO | mode, 0).
 */

#include "kernel/syscall/sys_mknod.hpp"

#include <stdint.h>

#include "kernel/errno.hpp"
#include "kernel/fs/path.hpp"
#include "kernel/ipc/fifo.hpp"
#include "kernel/syscall/path_util.hpp"

namespace cinux::syscall {

int64_t do_mknod_kernel(const char* resolved_path, uint32_t mode) {
    // This milestone: only FIFO creation (S_IFIFO) is supported. Char/block
    // device nodes are not implemented.
    if ((mode & 0xF000) != cinux::ipc::kSIfFifo) {
        return -kEnosys;
    }

    // FIFOs live in a flat in-memory namespace keyed by the path's leaf name
    // (a hobby-OS simplification: names are resolved under /dev by DevFS's
    // dynamic lookup).  The leaf is the tail after the last '/', NUL-terminated
    // as the tail of @p resolved_path.
    const char* leaf = resolved_path;
    for (const char* p = resolved_path; *p != '\0'; ++p) {
        if (*p == '/') {
            leaf = p + 1;
        }
    }
    if (*leaf == '\0') {
        return -kEinval;
    }

    auto r = cinux::ipc::FifoRegistry::instance().create(leaf);
    if (!r.ok()) {
        return -to_errno(r.error());
    }
    return 0;
}

int64_t sys_mknod(uint64_t path_virt, uint64_t mode, uint64_t /*dev*/, uint64_t, uint64_t,
                  uint64_t) {
    cinux::fs::PathBuf resolved;
    if (!resolve_user_path(path_virt, resolved.data())) {
        return -kEfault;
    }
    return do_mknod_kernel(resolved.data(), static_cast<uint32_t>(mode));
}

}  // namespace cinux::syscall
