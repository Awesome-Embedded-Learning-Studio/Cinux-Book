/**
 * @file kernel/syscall/sys_time.cpp
 * @brief sys_gettimeofday / sys_time handlers (gcc/g++ self-host batch, 2026-07-05)
 *
 * Both read CLOCK_REALTIME (RTC boot epoch + HPET monotonic delta -- the same
 * source as clock_gettime).  gettimeofday fills a struct timeval{tv_sec,
 * tv_usec}; the timezone argument is obsolete and ignored.  time() returns
 * the same seconds and optionally writes them through the user pointer.
 */

#include "kernel/syscall/sys_time.hpp"

#include <stdint.h>

#include "kernel/arch/x86_64/user_access.hpp"  // copy_to_user
#include "kernel/errno.hpp"
#include "kernel/syscall/sys_clock_gettime.hpp"  // do_clock_gettime_kernel, ktimespec

namespace cinux::syscall {

namespace {

constexpr uint64_t kClockRealtime = 0;

/// Linux struct timeval layout on x86-64: { time_t tv_sec; suseconds_t tv_usec; }.
struct ktimeval {
    int64_t tv_sec;
    int64_t tv_usec;
};

}  // namespace

int64_t sys_gettimeofday(uint64_t tv_virt, uint64_t /*tz_virt*/, uint64_t, uint64_t, uint64_t,
                         uint64_t) {
    ktimespec kts;
    int64_t   rc = do_clock_gettime_kernel(kClockRealtime, &kts);
    if (rc < 0) {
        return rc;
    }
    if (tv_virt != 0) {
        ktimeval tv;
        tv.tv_sec  = kts.tv_sec;
        tv.tv_usec = kts.tv_nsec / 1000;
        if (!cinux::user::copy_to_user(reinterpret_cast<void*>(tv_virt), &tv, sizeof(tv))) {
            return -cinux::kEfault;
        }
    }
    return 0;
}

int64_t sys_time(uint64_t t_virt, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    ktimespec kts;
    int64_t   rc = do_clock_gettime_kernel(kClockRealtime, &kts);
    if (rc < 0) {
        return rc;
    }
    if (t_virt != 0) {
        if (!cinux::user::copy_to_user(reinterpret_cast<void*>(t_virt), &kts.tv_sec,
                                       sizeof(kts.tv_sec))) {
            return -cinux::kEfault;
        }
    }
    return kts.tv_sec;
}

}  // namespace cinux::syscall
