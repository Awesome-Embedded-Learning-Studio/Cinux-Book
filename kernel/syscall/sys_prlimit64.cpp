/**
 * @file kernel/syscall/sys_prlimit64.cpp
 * @brief sys_prlimit64 handler (gcc/g++ self-host batch, 2026-07-05)
 *
 * glibc's malloc probes RLIMIT_AS / RLIMIT_DATA via prlimit64 at startup to
 * size its arenas.  Cinux enforces no resource limits (brk/mmap grow on
 * demand up to the user VA window), so every limit is reported as unlimited.
 * The new_rlim argument is accepted but ignored -- there is no enforcement
 * path.  pid/resource validation is intentionally loose: callers either pass
 * 0 (self) or a resource the libc enumerates; we never deny.
 */

#include "kernel/syscall/sys_prlimit64.hpp"

#include <stdint.h>

#include "kernel/arch/x86_64/user_access.hpp"  // copy_to_user
#include "kernel/errno.hpp"

namespace cinux::syscall {

namespace {

/// Linux struct rlimit layout on x86-64: { rlim_t rlim_cur; rlim_t rlim_max; }.
struct krlimit {
    uint64_t rlim_cur;
    uint64_t rlim_max;
};
constexpr uint64_t kRlimInfinite = ~0ULL;

}  // namespace

int64_t sys_prlimit64(uint64_t /*pid*/, uint64_t /*resource*/, uint64_t /*new_rlim_virt*/,
                      uint64_t old_rlim_virt, uint64_t, uint64_t) {
    if (old_rlim_virt != 0) {
        krlimit lim{kRlimInfinite, kRlimInfinite};
        if (!cinux::user::copy_to_user(reinterpret_cast<void*>(old_rlim_virt), &lim, sizeof(lim))) {
            return -cinux::kEfault;
        }
    }
    return 0;
}

}  // namespace cinux::syscall
