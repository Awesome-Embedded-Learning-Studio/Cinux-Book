/**
 * @file kernel/syscall/poll_core.hpp
 * @brief Shared poll(2)/select(2) engine (F8-M5 real poll)
 *
 * Both sys_poll and sys_select funnel through do_poll_core(): walk an in-kernel
 * pollfd array, compute each fd's revents via InodeOps::poll_events(), and block
 * until one is ready or the timeout elapses.  select() just converts its fd_set
 * bitmaps into a pollfd[] and back.
 *
 * Blocking (event-driven, #DF-safe -- no sti/hlt in the syscall path):
 *  - timeout < 0 (infinite): park on every pollable fd's wait queue via the
 *    scheduler's prepare_to_wait() + schedule_blocked() (the same triplet a
 *    blocked pipe read / socket recv uses).  A producer on any of those fds
 *    (pipe write, socket on_data, ...) wakes the poller; it detaches and
 *    re-scans.  No spin.
 *  - timeout == 0: a single readiness pass; never blocks.
 *  - timeout > 0: yield between passes until a fd is ready or the HPET/PIT
 *    deadline passes.  A real timer-wake -- waking a PARKED poller on timeout
 *    without yielding -- is the F5-M4 timer follow-up (DEBT); until then a
 *    finite timeout yields (giving other tasks the CPU) rather than hangs.
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>

namespace cinux::syscall {

/// Linux struct pollfd (x86-64): { int fd; short events; short revents; } = 8 B.
/// Laid out for direct copy_to/from_user against the userspace pollfd.
struct kpollfd {
    int32_t fd;
    int16_t events;
    int16_t revents;
};

/**
 * @brief Core poll/select engine.
 *
 * @param pfds       In-kernel pollfd array (caller staged it in from user space).
 * @param nfds       Number of entries in @p pfds.
 * @param timeout_ms < 0 = block until a fd is ready; 0 = poll once; > 0 = wait
 *                   up to that many milliseconds.
 * @return Number of fds with non-zero revents (0 on timeout / nothing ready).
 *         Fills each pfd's @c revents.  Regular/always-ready fds (and fds <= 2
 *         with no fd-table entry, i.e. the legacy console) report POLLIN|POLLOUT;
 *         an absent fd reports POLLNVAL.
 */
int64_t do_poll_core(kpollfd* pfds, uint64_t nfds, int64_t timeout_ms);

}  // namespace cinux::syscall
