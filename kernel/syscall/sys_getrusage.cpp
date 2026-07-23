/**
 * @file kernel/syscall/sys_getrusage.cpp
 * @brief sys_getrusage handler (F-ECO batch 5)
 *
 * See sys_getrusage.hpp.  Zeros today -- no accounting subsystem yet.  Validating
 * @p who keeps the shape faithful so a real accounting milestone just fills the
 * fields without touching the syscall boundary.
 *
 * Namespace: cinux::syscall
 */

#include "kernel/syscall/sys_getrusage.hpp"

#include <stdint.h>

#include "kernel/arch/x86_64/user_access.hpp"  // copy_to_user (SMAP/extable)
#include "kernel/errno.hpp"

namespace cinux::syscall {

int64_t do_getrusage_kernel(uint64_t who, krusage* out) {
    if (out == nullptr) {
        return -cinux::kEinval;
    }
    // RUSAGE_SELF / RUSAGE_CHILDREN / RUSAGE_THREAD.  The kernel (SELF=1,
    // CHILDREN=-1, THREAD=1) and historical libc (SELF=0) conventions disagree
    // on the numbers, so accept {-1, 0, 1} -- the union -- to never reject a
    // legitimate caller; anything else is -EINVAL (the data is zeros regardless).
    if (who != static_cast<uint64_t>(-1) && who != 0 && who != 1) {
        return -cinux::kEinval;
    }
    *out = krusage{};  // zero: no per-task resource accounting yet
    return 0;
}

int64_t sys_getrusage(uint64_t who, uint64_t usage_virt, uint64_t, uint64_t, uint64_t, uint64_t) {
    krusage ru;
    int64_t rc = do_getrusage_kernel(who, &ru);
    if (rc < 0) {
        return rc;
    }
    if (!cinux::user::copy_to_user(reinterpret_cast<void*>(usage_virt), &ru, sizeof(ru))) {
        return -cinux::kEfault;
    }
    return 0;
}

}  // namespace cinux::syscall
