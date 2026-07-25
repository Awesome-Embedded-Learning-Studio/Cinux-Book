/**
 * @file kernel/syscall/sys_poll.cpp
 * @brief sys_poll handler (F8-M5 real poll)
 *
 * A thin user-boundary wrapper over do_poll_core(): stage the pollfd array into
 * a kernel buffer, run the shared blocking engine, stage revents back out.  The
 * per-fd readiness checks and the prepare_to_wait/schedule_blocked blocking live
 * in poll_core.cpp (shared with sys_select).
 *
 * The pollfd array is stack-staged (capped at kPollMaxFds entries -- 512 B),
 * enough for real apps (sh polls stdin; nc/wget poll a socket); a larger nfds is
 * -EINVAL rather than a heap alloc.  poll(NULL, 0, timeout) is a portable sleep
 * and is honoured (do_poll_core with an empty set).
 *
 * Namespace: cinux::syscall
 */

#include "kernel/syscall/sys_poll.hpp"

#include <stdint.h>

#include "kernel/arch/x86_64/user_access.hpp"  // copy_to/from_user
#include "kernel/errno.hpp"
#include "kernel/syscall/poll_core.hpp"  // do_poll_core, kpollfd

namespace cinux::syscall {

/// Stack-staged pollfd cap.  Plenty for real apps; a larger nfds is -EINVAL.
constexpr uint64_t kPollMaxFds = 64;

int64_t sys_poll(uint64_t fds_virt, uint64_t nfds, uint64_t timeout, uint64_t, uint64_t, uint64_t) {
    if (nfds > kPollMaxFds) {
        return -cinux::kEinval;
    }
    if (nfds == 0) {
        // poll(NULL, 0, timeout): a portable sub-second sleep.  No array to copy.
        return do_poll_core(nullptr, 0, static_cast<int64_t>(timeout));
    }
    if (fds_virt == 0) {
        return -cinux::kEfault;
    }

    kpollfd kfds[kPollMaxFds];
    if (!cinux::user::copy_from_user(kfds, reinterpret_cast<void*>(fds_virt),
                                     nfds * sizeof(kpollfd))) {
        return -cinux::kEfault;
    }

    int64_t ready = do_poll_core(kfds, nfds, static_cast<int64_t>(timeout));

    if (!cinux::user::copy_to_user(reinterpret_cast<void*>(fds_virt), kfds,
                                   nfds * sizeof(kpollfd))) {
        return -cinux::kEfault;
    }
    return ready;
}

}  // namespace cinux::syscall
