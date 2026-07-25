/**
 * @file kernel/syscall/sys_select.cpp
 * @brief sys_select handler (F8-M5)
 *
 * Translates the select() fd_set bitmaps into a pollfd[] and runs do_poll_core()
 * -- the same engine sys_poll uses -- then translates the returned revents back
 * into the three output fd_sets.  Sharing the engine means poll and select have
 * one blocking path (infinite = event sleep; finite = yield until deadline).
 *
 * fd_set layout: on little-endian x86-64 a bit at position fd lives at
 * byte[fd/8], bit (fd % 8), matching libc's unsigned-long-word array.  The user
 * fd_set is FD_SETSIZE = 1024 bits (128 B), but a process can hold at most
 * FD_TABLE_SIZE = 256 fds, so only the low 32 bytes (bits 0..255) are
 * meaningful; this handler copies and scans exactly that span, keeping the stack
 * working set tiny.  @p nfds bounds the scan (clamped to 256).
 *
 * Timeout: a non-NULL struct timeval { sec, usec } -> ms; NULL = block forever.
 * On return the timeval is updated to the time remaining (Linux behaviour).
 *
 * Namespace: cinux::syscall
 */

#include "kernel/syscall/sys_select.hpp"

#include <stdint.h>

#include "kernel/arch/x86_64/user_access.hpp"  // copy_to/from_user
#include "kernel/drivers/hpet/hpet.hpp"
#include "kernel/drivers/pit/pit.hpp"  // monotonic fallback when HPET is absent
#include "kernel/errno.hpp"
#include "kernel/fs/file.hpp"   // FD_TABLE_SIZE
#include "kernel/fs/inode.hpp"  // kPoll* event bits
#include "kernel/syscall/poll_core.hpp"

namespace cinux::syscall {

namespace {

/// Meaningful fd_set span: a process cannot hold >= FD_TABLE_SIZE fds, so only
/// bits 0..FD_TABLE_SIZE-1 can ever be ready.  32 B on x86-64 (256 bits).
constexpr uint64_t kSetBytes   = cinux::fs::FD_TABLE_SIZE / 8;
constexpr uint64_t kPollMaxFds = 64;  ///< stack-staged pollfd cap (watched fds)
constexpr uint64_t kNsPerMs    = 1'000'000ULL;
constexpr uint64_t kNsPerSec   = 1'000'000'000ULL;

/// Linux x86-64 struct timeval { time_t tv_sec; suseconds_t tv_usec; } (16 B).
struct ktimeval {
    int64_t tv_sec;
    int64_t tv_usec;
};

uint64_t monotonic_ns() {
    if (cinux::drivers::g_hpet.available()) {
        return cinux::drivers::g_hpet.monotonic_ns();
    }
    return cinux::drivers::PIT::get_uptime_ms() * kNsPerMs;
}

bool fd_is_set(const uint8_t* set, int fd) {
    return (set[fd / 8] & (1u << (fd % 8))) != 0;
}

void fd_set_bit(uint8_t* set, int fd) {
    set[fd / 8] |= static_cast<uint8_t>(1u << (fd % 8));
}

}  // namespace

int64_t sys_select(uint64_t nfds, uint64_t readfds, uint64_t writefds, uint64_t exceptfds,
                   uint64_t timeout_virt, uint64_t) {
    // nfds bounds the scan; clamp to FD_TABLE_SIZE (fds >= that cannot exist).
    if (nfds > cinux::fs::FD_TABLE_SIZE) {
        nfds = cinux::fs::FD_TABLE_SIZE;
    }

    // Copy in the requested sets' low span (NULL -> not watched).  Reused below
    // as the OUTPUT sets (rebuilt from revents), so no second buffer is needed.
    uint8_t rd[kSetBytes] = {};
    uint8_t wr[kSetBytes] = {};
    uint8_t ex[kSetBytes] = {};
    if (readfds != 0 &&
        !cinux::user::copy_from_user(rd, reinterpret_cast<void*>(readfds), kSetBytes)) {
        return -cinux::kEfault;
    }
    if (writefds != 0 &&
        !cinux::user::copy_from_user(wr, reinterpret_cast<void*>(writefds), kSetBytes)) {
        return -cinux::kEfault;
    }
    if (exceptfds != 0 &&
        !cinux::user::copy_from_user(ex, reinterpret_cast<void*>(exceptfds), kSetBytes)) {
        return -cinux::kEfault;
    }

    // Resolve the timeout: NULL -> infinite (-1); {sec,usec} -> ms (+ deadline).
    int64_t  timeout_ms   = -1;
    bool     has_deadline = false;
    uint64_t deadline     = 0;
    if (timeout_virt != 0) {
        ktimeval tv;
        if (!cinux::user::copy_from_user(&tv, reinterpret_cast<void*>(timeout_virt), sizeof(tv))) {
            return -cinux::kEfault;
        }
        if (tv.tv_sec < 0 || tv.tv_usec < 0 ||
            tv.tv_usec >= static_cast<int64_t>(kNsPerSec / kNsPerMs)) {
            return -cinux::kEinval;
        }
        timeout_ms   = tv.tv_sec * 1000 + tv.tv_usec / 1000;
        has_deadline = true;
        deadline     = monotonic_ns() + static_cast<uint64_t>(timeout_ms) * kNsPerMs;
    }

    // Translate the set bits into a pollfd[] (read->POLLIN, write->POLLOUT,
    // except->POLLPRI).  Sparse sets yield only the watched fds.
    kpollfd  pfds[kPollMaxFds];
    uint64_t count = 0;
    for (uint64_t fd = 0; fd < nfds; ++fd) {
        uint16_t ev = 0;
        if (readfds != 0 && fd_is_set(rd, static_cast<int>(fd))) {
            ev |= cinux::fs::kPollIn;
        }
        if (writefds != 0 && fd_is_set(wr, static_cast<int>(fd))) {
            ev |= cinux::fs::kPollOut;
        }
        if (exceptfds != 0 && fd_is_set(ex, static_cast<int>(fd))) {
            ev |= cinux::fs::kPollPri;
        }
        if (ev == 0) {
            continue;
        }
        if (count >= kPollMaxFds) {
            return -cinux::kEinval;  // too many watched fds for the stack cap
        }
        pfds[count].fd      = static_cast<int32_t>(fd);
        pfds[count].events  = static_cast<int16_t>(ev);
        pfds[count].revents = 0;
        ++count;
    }

    int64_t ready = do_poll_core(pfds, count, timeout_ms);

    // Rebuild the output sets IN PLACE (zero, then set ready bits).  POLLHUP/
    // POLLERR on a watched read fd surface in the read set so the app wakes and
    // reads EOF / gets the error (Linux reports them this way).
    for (uint64_t i = 0; i < kSetBytes; ++i) {
        rd[i] = 0;
        wr[i] = 0;
        ex[i] = 0;
    }
    for (uint64_t i = 0; i < count; ++i) {
        uint16_t rv = static_cast<uint16_t>(pfds[i].revents);
        int      fd = pfds[i].fd;
        if (rv == 0) {
            continue;
        }
        if (readfds != 0 &&
            (rv & (cinux::fs::kPollIn | cinux::fs::kPollHup | cinux::fs::kPollErr))) {
            fd_set_bit(rd, fd);
        }
        if (writefds != 0 && (rv & cinux::fs::kPollOut)) {
            fd_set_bit(wr, fd);
        }
        if (exceptfds != 0 && (rv & cinux::fs::kPollPri)) {
            fd_set_bit(ex, fd);
        }
    }

    if (readfds != 0 &&
        !cinux::user::copy_to_user(reinterpret_cast<void*>(readfds), rd, kSetBytes)) {
        return -cinux::kEfault;
    }
    if (writefds != 0 &&
        !cinux::user::copy_to_user(reinterpret_cast<void*>(writefds), wr, kSetBytes)) {
        return -cinux::kEfault;
    }
    if (exceptfds != 0 &&
        !cinux::user::copy_to_user(reinterpret_cast<void*>(exceptfds), ex, kSetBytes)) {
        return -cinux::kEfault;
    }

    // Update *timeout to the time remaining (Linux updates it; never increases).
    if (has_deadline) {
        uint64_t now    = monotonic_ns();
        int64_t  rem_ns = (now >= deadline) ? 0 : static_cast<int64_t>(deadline - now);
        ktimeval tv;
        tv.tv_sec  = rem_ns / static_cast<int64_t>(kNsPerSec);
        tv.tv_usec = (rem_ns % static_cast<int64_t>(kNsPerSec)) / 1000;
        static_cast<void>(cinux::user::copy_to_user(reinterpret_cast<void*>(timeout_virt), &tv, sizeof(tv)));
    }

    return ready;
}

}  // namespace cinux::syscall
