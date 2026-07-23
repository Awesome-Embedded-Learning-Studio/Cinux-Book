/**
 * @file kernel/syscall/sys_poll.cpp
 * @brief sys_poll STUB handler (F-ECO busybox sh smoke)
 *
 * See sys_poll.hpp.  For each pollfd with fd >= 0, set revents = events & POLLIN
 * (report readable).  No waiting -- always returns immediately.  This is enough
 * for busybox `sh`'s poll-then-read input loop: poll says "ready", sh calls
 * read(), which blocks on the console TTY until the user types.  A real poll
 * (blocking, wait-queue driven) is F8-M5.
 *
 * The pollfd array crosses the user boundary via copy_to/from_user, staged
 * through a kernel buffer to avoid a large user length on the kernel stack.
 *
 * Namespace: cinux::syscall
 */

#include "kernel/syscall/sys_poll.hpp"

#include <stdint.h>

#include "kernel/arch/x86_64/user_access.hpp"  // copy_to/from_user
#include "kernel/errno.hpp"

namespace cinux::syscall {

namespace {
constexpr uint16_t kPollin = 0x0001;  ///< POLLIN (data available)

/// Linux struct pollfd (x86-64): { int fd; short events; short revents; } = 8 B.
struct kpollfd {
    int32_t fd;
    int16_t events;
    int16_t revents;
};
}  // namespace

int64_t sys_poll(uint64_t fds_virt, uint64_t nfds, uint64_t /*timeout*/, uint64_t, uint64_t,
                 uint64_t) {
    if (nfds > 16) {
        return -cinux::kEinval;  // bounded -- stack-staging cap; plenty for sh's stdin
    }
    if (nfds == 0 || fds_virt == 0) {
        return 0;  // nothing to poll
    }
    kpollfd kfds[16];
    if (!cinux::user::copy_from_user(kfds, reinterpret_cast<void*>(fds_virt),
                                     nfds * sizeof(kpollfd))) {
        return -cinux::kEfault;
    }
    int64_t ready = 0;
    for (uint64_t i = 0; i < nfds; ++i) {
        if (kfds[i].fd < 0) {
            kfds[i].revents = 0;  // fd < 0 -> ignored, revents untouched (Linux)
            continue;
        }
        // STUB: always report POLLIN if the caller asked for it.  read() on the
        // console TTY then blocks until real input, so no spin.
        kfds[i].revents = static_cast<int16_t>(kfds[i].events & kPollin);
        ++ready;
    }
    if (!cinux::user::copy_to_user(reinterpret_cast<void*>(fds_virt), kfds,
                                   nfds * sizeof(kpollfd))) {
        return -cinux::kEfault;
    }
    return ready;
}

}  // namespace cinux::syscall
