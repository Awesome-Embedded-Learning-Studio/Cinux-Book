/**
 * @file kernel/syscall/sys_sysinfo.cpp
 * @brief sys_sysinfo handler (F-ECO batch 5)
 *
 * See sys_sysinfo.hpp.  monotonic_ns() mirrors sys_clock_gettime / sys_nanosleep
 * (HPET + PIT fallback).  RAM fields come straight off g_pmm; procs walks the
 * task registry via the ProcFS nth accessor.
 *
 * Namespace: cinux::syscall
 */

#include "kernel/syscall/sys_sysinfo.hpp"

#include <stdint.h>

#include "kernel/arch/x86_64/user_access.hpp"  // copy_to_user (SMAP/extable)
#include "kernel/drivers/hpet/hpet.hpp"
#include "kernel/drivers/pit/pit.hpp"  // monotonic fallback when HPET is absent
#include "kernel/errno.hpp"
#include "kernel/mm/pmm.hpp"       // g_pmm.total/free_page_count()
#include "kernel/proc/signal.hpp"  // signal_nth_task_pid (task count)

namespace cinux::syscall {

namespace {

constexpr uint64_t kNsPerSec = 1'000'000'000ULL;
constexpr uint64_t kNsPerMs  = 1'000'000ULL;
constexpr uint64_t kPageSize = 4096;  ///< x86-64 4K pages (paging_config.hpp)

/// Boot-relative monotonic nanoseconds, HPET-backed with a PIT fallback.
uint64_t monotonic_ns() {
    if (cinux::drivers::g_hpet.available()) {
        return cinux::drivers::g_hpet.monotonic_ns();
    }
    return cinux::drivers::PIT::get_uptime_ms() * kNsPerMs;
}

}  // anonymous namespace

int64_t do_sysinfo_kernel(ksysinfo* out) {
    if (out == nullptr) {
        return -cinux::kEinval;
    }
    *out           = ksysinfo{};  // zero everything; untracked fields (loads/swap/high) read 0
    out->uptime    = static_cast<int64_t>(monotonic_ns() / kNsPerSec);
    out->totalram  = cinux::mm::g_pmm.total_page_count() * kPageSize;
    out->freeram   = cinux::mm::g_pmm.free_page_count() * kPageSize;
    // Count live tasks by walking the registry to successive indices.  PID_MAX
    // is small (256-ish), so this is bounded; the guard is a backstop.
    uint16_t count = 0;
    int      pid   = 0;
    while (cinux::proc::signal_nth_task_pid(count, &pid)) {
        if (count == 0xFFFFU) {
            break;
        }
        ++count;
    }
    out->procs   = count;
    out->memunit = 1;  // RAM fields are bytes
    return 0;
}

int64_t sys_sysinfo(uint64_t info_virt, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    if (info_virt == 0) {
        return -cinux::kEfault;
    }
    ksysinfo ki;
    int64_t  rc = do_sysinfo_kernel(&ki);
    if (rc < 0) {
        return rc;
    }
    if (!cinux::user::copy_to_user(reinterpret_cast<void*>(info_virt), &ki, sizeof(ki))) {
        return -cinux::kEfault;
    }
    return 0;
}

}  // namespace cinux::syscall
