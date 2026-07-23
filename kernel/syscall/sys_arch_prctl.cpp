/**
 * @file kernel/syscall/sys_arch_prctl.cpp
 * @brief sys_arch_prctl handler implementation (F10-M1 batch 4)
 *
 * Handles ARCH_SET_FS / ARCH_GET_FS (the FS base, used for TLS).  GS base
 * and the sub-options are not used by musl and return -EINVAL.  The new FS
 * base is recorded into the current task's CpuContext so a later context
 * switch restores it; set_tls_base() also programs the MSR immediately so
 * subsequent %fs-relative accesses in the syscall return path see it.
 */

#include "kernel/syscall/sys_arch_prctl.hpp"

#include <stdint.h>

#include "kernel/arch/x86_64/tls.hpp"
#include "kernel/errno.hpp"
#include "kernel/proc/scheduler.hpp"

namespace cinux::syscall {

namespace {

// arch_prctl option codes (Linux x86-64 UAPI).
constexpr uint64_t kArchSetGs = 0x1001;
constexpr uint64_t kArchSetFs = 0x1002;
constexpr uint64_t kArchGetGs = 0x1003;
constexpr uint64_t kArchGetFs = 0x1004;

}  // anonymous namespace

int64_t sys_arch_prctl(uint64_t code, uint64_t addr, uint64_t, uint64_t, uint64_t, uint64_t) {
    auto* task = cinux::proc::Scheduler::current();

    switch (code) {
    case kArchSetFs: {
        // Program the FS base now and remember it for context switches.
        cinux::arch::set_tls_base(addr);
        if (task != nullptr) {
            task->ctx.fs_base = addr;
        }
        return 0;
    }
    case kArchGetFs: {
        // arch_prctl(GET_FS, &addr) writes the base into a user word.
        if (task == nullptr) {
            return -cinux::kEfault;
        }
        auto* out = reinterpret_cast<uint64_t*>(addr);
        *out      = cinux::arch::get_tls_base();
        return 0;
    }
    default:
        // GS base / unsupported sub-options: musl never uses these.
        return -cinux::kEinval;
    }
}

}  // namespace cinux::syscall
