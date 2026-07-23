/**
 * @file kernel/syscall/sys_nanosleep.cpp
 * @brief sys_nanosleep handler (F-ECO batch 3)
 *
 * See sys_nanosleep.hpp.  monotonic_ns() mirrors sys_clock_gettime's HPET/PIT
 * fallback (duplicated -- a 5-line helper -- so the two handlers stay
 * independently linkable; nanosleep pulls in no clock_gettime .cpp).
 *
 * Namespace: cinux::syscall
 */

#include "kernel/syscall/sys_nanosleep.hpp"

#include <stdint.h>

#include "kernel/arch/x86_64/user_access.hpp"  // copy_to/from_user (SMAP/extable)
#include "kernel/drivers/hpet/hpet.hpp"
#include "kernel/drivers/pit/pit.hpp"  // monotonic fallback when HPET is absent
#include "kernel/errno.hpp"
#include "kernel/proc/scheduler.hpp"  // Scheduler::yield

namespace cinux::syscall {

namespace {

constexpr uint64_t kNsPerSec = 1'000'000'000ULL;
constexpr uint64_t kNsPerMs  = 1'000'000ULL;

/// Boot-relative monotonic nanoseconds, HPET-backed with a PIT fallback.
uint64_t monotonic_ns() {
    if (cinux::drivers::g_hpet.available()) {
        return cinux::drivers::g_hpet.monotonic_ns();
    }
    return cinux::drivers::PIT::get_uptime_ms() * kNsPerMs;
}

}  // anonymous namespace

int64_t do_nanosleep_kernel(const ktimespec* req, ktimespec* rem) {
    if (req == nullptr) {
        return -cinux::kEinval;
    }
    if (req->tv_sec < 0 || req->tv_nsec < 0 || req->tv_nsec >= static_cast<int64_t>(kNsPerSec)) {
        return -cinux::kEinval;  // negative or out-of-range nsec
    }
    const uint64_t target_ns =
        static_cast<uint64_t>(req->tv_sec) * kNsPerSec + static_cast<uint64_t>(req->tv_nsec);
    const uint64_t deadline = monotonic_ns() + target_ns;

    // Poll the monotonic counter until the deadline, yielding between checks.
    // NOT sti/hlt (the #DF hazard), NOT an IRQ wake (the F5-M4 follow-up); the
    // yield gives other tasks the CPU so this is not a hard spin.
    while (monotonic_ns() < deadline) {
        cinux::proc::Scheduler::yield();
    }

    if (rem != nullptr) {
        rem->tv_sec  = 0;
        rem->tv_nsec = 0;  // fully slept; no signal-interruption path yet
    }
    return 0;
}

int64_t sys_nanosleep(uint64_t req_virt, uint64_t rem_virt, uint64_t, uint64_t, uint64_t,
                      uint64_t) {
    if (req_virt == 0) {
        return -cinux::kEfault;
    }
    ktimespec req;
    if (!cinux::user::copy_from_user(&req, reinterpret_cast<void*>(req_virt), sizeof(req))) {
        return -cinux::kEfault;
    }
    ktimespec rem_local{0, 0};
    int64_t   rc = do_nanosleep_kernel(&req, &rem_local);
    if (rc < 0) {
        return rc;
    }
    if (rem_virt != 0) {
        // Best-effort: a NULL rem is allowed (caller does not care about remaining).
        (void)cinux::user::copy_to_user(reinterpret_cast<void*>(rem_virt), &rem_local,
                                        sizeof(rem_local));
    }
    return 0;
}

}  // namespace cinux::syscall
