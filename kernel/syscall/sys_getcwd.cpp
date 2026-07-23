/**
 * @file kernel/syscall/sys_getcwd.cpp
 * @brief sys_getcwd handler (P0e SMAP-layered)
 *
 * Layered: do_getcwd_kernel writes the cwd into a KERNEL buffer; sys_getcwd
 * is the boundary that copy_to_user's it out. The old memcpy into the user
 * buffer is gone.
 */

#include "kernel/syscall/sys_getcwd.hpp"

#include <stdint.h>

#include "kernel/arch/x86_64/user_access.hpp"  // P0e (SMAP): copy_to_user
#include "kernel/errno.hpp"
#include "kernel/fs/path.hpp"  // PathBuf
#include "kernel/lib/string.hpp"
#include "kernel/proc/scheduler.hpp"

namespace cinux::syscall {

int64_t do_getcwd_kernel(char* dst, uint32_t size) {
    if (size == 0) {
        return -cinux::kEinval;
    }
    cinux::proc::Task* current = cinux::proc::Scheduler::current();
    if (current == nullptr) {
        return -cinux::kEsrch;
    }
    const char* cwd_str = (current->cwd != nullptr) ? current->cwd->path : "";
    uint32_t    cwd_len = 0;
    while (cwd_str[cwd_len] != '\0') {
        ++cwd_len;
    }
    ++cwd_len;  // include NUL
    if (cwd_len > size) {
        return -cinux::kErange;
    }
    memcpy(dst, cwd_str, cwd_len);
    return static_cast<int64_t>(cwd_len);
}

int64_t sys_getcwd(uint64_t buf_virt, uint64_t size, uint64_t, uint64_t, uint64_t, uint64_t) {
    if (!cinux::user::access_ok(reinterpret_cast<void*>(buf_virt), size)) {
        return -cinux::kEfault;
    }
    // Stage into a kernel PATH_MAX buffer (cwd cannot exceed PATH_MAX).
    cinux::fs::PathBuf kpath;
    uint32_t           stage =
        (size < cinux::fs::PATH_MAX) ? static_cast<uint32_t>(size) : cinux::fs::PATH_MAX;
    int64_t n = do_getcwd_kernel(kpath.data(), stage);
    if (n < 0) {
        return n;
    }
    if (!cinux::user::copy_to_user(reinterpret_cast<void*>(buf_virt), kpath.data(),
                                   static_cast<uint64_t>(n))) {
        return -cinux::kEfault;
    }
    return n;
}

}  // namespace cinux::syscall
