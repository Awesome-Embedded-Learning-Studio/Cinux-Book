/**
 * @file kernel/syscall/sys_linux_stubs.cpp
 * @brief Linux ABI probing stubs (gcc/g++ self-host syscall batch, 2026-07-05)
 *
 * See the header for the rationale.  Each handler returns just enough for the
 * probing libc to fall back gracefully:
 *   - rseq(334)    -> -ENOSYS: glibc gives up the restartable-sequence path.
 *   - clone3(435)  -> -ENOSYS: glibc falls back to clone/fork.
 *   - set_robust_list(273) -> 0: the robust-futex probe is satisfied.  We do
 *     not truly clean up robust locks on exit, but no compile/load path uses
 *     them (pthread-only); a future pthread batch would wire the cleanup.
 *   - sendfile(40) -> -ENOSYS: cp/copy tools fall back to read+write.
 */

#include "kernel/syscall/sys_linux_stubs.hpp"

#include "kernel/arch/x86_64/user_access.hpp"  // copy_to/from_user (setitimer / affinity)
#include "kernel/drivers/acpi/acpi.hpp"        // g_acpi_info (cpu_count)
#include "kernel/errno.hpp"
#include "kernel/proc/process.hpp"  // Task::itimer_real_*
#include "kernel/proc/scheduler.hpp"  // Scheduler::current()
#include "kernel/proc/signal.hpp"  // signal_find_task_by_pid / signal_send (tkill)
#include "kernel/proc/sync.hpp"  // InterruptGuard (setitimer field update)

namespace cinux::syscall {

// getcpu(309): glibc probes it for per-CPU affinity hints via vDSO; the syscall
// fallback returning -ENOSYS makes glibc give up the hint gracefully.
int64_t sys_getcpu(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    return -cinux::kEnosys;
}

int64_t sys_rseq(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    return -cinux::kEnosys;
}

int64_t sys_clone3(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    return -cinux::kEnosys;
}

int64_t sys_set_robust_list(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    return 0;
}

int64_t sys_sendfile(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    return -cinux::kEnosys;
}

// tkill(200): send a signal to a task by tid.  busybox sh's job control uses
// this to forward SIGINT (Ctrl+C from the PTY) to the foreground child (e.g.
// ping).  Cinux tasks are single-threaded (tid == pid), so reuse the same
// pid->Task lookup + signal_send path as sys_kill(62).
int64_t sys_tkill(uint64_t tid, uint64_t sig, uint64_t, uint64_t, uint64_t, uint64_t) {
    auto* t = cinux::proc::signal_find_task_by_pid(static_cast<int>(tid));
    if (t == nullptr) {
        return -cinux::kEsrch;
    }
    return cinux::proc::signal_send(t, static_cast<cinux::proc::Signal>(sig));
}

// Linux x86-64 struct itimerval { struct timeval it_interval; struct timeval it_value; }
// where struct timeval { long tv_sec; long tv_usec; } (16 B + 16 B = 32 B).
namespace {
struct TimevalStub {
    int64_t tv_sec;
    int64_t tv_usec;
};
struct ItimervalStub {
    TimevalStub it_interval;
    TimevalStub it_value;
};

/// Convert a Linux timeval to nanoseconds.  Negative / out-of-range usec is
/// treated as 0 (disarm) -- matches the "no timer" intent of a malformed value.
uint64_t timeval_to_ns(const TimevalStub& tv) {
    if (tv.tv_sec < 0 || tv.tv_usec < 0 || tv.tv_usec >= 1'000'000) {
        return 0;
    }
    return static_cast<uint64_t>(tv.tv_sec) * 1'000'000'000ULL +
           static_cast<uint64_t>(tv.tv_usec) * 1'000ULL;
}
void ns_to_timeval(uint64_t ns, TimevalStub& tv) {
    tv.tv_sec  = static_cast<int64_t>(ns / 1'000'000'000ULL);
    tv.tv_usec = static_cast<int64_t>((ns % 1'000'000'000ULL) / 1'000ULL);
}
}  // namespace

// setitimer(38): busybox ping arms ITIMER_REAL (1 s interval) so SIGALRM fires
// every second to send the next echo.  We store it_value/it_interval on the task
// (in ns); the PIT IRQ's itimer_real_tick() decrements and queues SIGALRM on
// expiry, reloading from it_interval.  Only ITIMER_REAL is supported (the only
// one busybox/glibc probe); ITIMER_VIRTUAL / ITIMER_PROF return -EINVAL.
int64_t sys_setitimer(uint64_t which, uint64_t new_value, uint64_t old_value, uint64_t,
                      uint64_t, uint64_t) {
    constexpr uint64_t kItimerReal = 0;
    if (which != kItimerReal) {
        return -cinux::kEinval;
    }
    cinux::proc::Task* self = cinux::proc::Scheduler::current();
    if (self == nullptr) {
        return -cinux::kEinval;
    }

    // Return the previous setting before installing the new one.
    if (old_value != 0) {
        ItimervalStub oldv{};
        ns_to_timeval(self->itimer_real_value_ns, oldv.it_value);
        ns_to_timeval(self->itimer_real_interval_ns, oldv.it_interval);
        if (!cinux::user::copy_to_user(reinterpret_cast<void*>(old_value), &oldv, sizeof(oldv))) {
            return -cinux::kEfault;
        }
    }
    if (new_value == 0) {
        return -cinux::kEfault;  // Linux: new_value must be non-NULL
    }
    ItimervalStub newv{};
    if (!cinux::user::copy_from_user(&newv, reinterpret_cast<const void*>(new_value), sizeof(newv))) {
        return -cinux::kEfault;
    }
    // Install under a brief IRQ guard so a concurrent PIT tick on this CPU does
    // not read a half-updated (value, interval) pair.  it_value == 0 disarms.
    {
        cinux::proc::InterruptGuard guard;
        self->itimer_real_value_ns    = timeval_to_ns(newv.it_value);
        self->itimer_real_interval_ns = timeval_to_ns(newv.it_interval);
    }
    return 0;
}

// sched_getaffinity(204): busybox `nproc` + glibc probe it to learn the online
// CPU set.  Cinux marks every online CPU (BSP + APs from g_acpi_info) eligible,
// so the mask has cpu_count low bits set and nproc reports the real topology.
// Returns the byte count placed (Linux raw-syscall contract; glibc/musl wrappers
// translate non-negative to 0).  pid is ignored -- Cinux has one global affinity.
int64_t sys_sched_getaffinity(uint64_t /*pid*/, uint64_t cpusetsize, uint64_t mask_virt,
                               uint64_t, uint64_t, uint64_t) {
    if (mask_virt == 0 || cpusetsize == 0) {
        return -cinux::kEfault;
    }
    const auto& info = cinux::drivers::acpi::g_acpi_info;
    uint32_t    n    = info.cpu_count > 0 ? info.cpu_count : 1;
    uint8_t     buf[128] = {0};  // up to 1024 CPUs
    for (uint32_t i = 0; i < n && i < sizeof(buf) * 8; ++i) {
        buf[i / 8] |= static_cast<uint8_t>(1u << (i % 8));
    }
    uint64_t bytes = (static_cast<uint64_t>(n) + 7) / 8;
    if (bytes > cpusetsize) {
        bytes = cpusetsize;
    }
    if (!cinux::user::copy_to_user(reinterpret_cast<void*>(mask_virt), buf, bytes)) {
        return -cinux::kEfault;
    }
    return static_cast<int64_t>(bytes);
}

}  // namespace cinux::syscall
