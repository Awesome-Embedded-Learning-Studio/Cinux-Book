/**
 * @file kernel/syscall/sys_waitpid.cpp
 * @brief sys_waitpid handler implementation
 *
 * Delegates to cinux::proc::waitpid() which searches the caller's
 * children list for a zombie child, collects the exit status, and
 * reaps the child TCB.
 */

#include "kernel/syscall/sys_waitpid.hpp"

#include "kernel/arch/x86_64/user_access.hpp"  // P0 (SMAP): put_user
#include "kernel/errno.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/proc/pid.hpp"
#include "kernel/proc/process.hpp"

namespace cinux::syscall {

int64_t sys_waitpid(uint64_t pid_arg, uint64_t status_arg, uint64_t options_arg, uint64_t, uint64_t,
                    uint64_t) {
    int  pid     = static_cast<int>(pid_arg);
    int* status  = reinterpret_cast<int*>(status_arg);  // user pointer
    int  options = static_cast<int>(options_arg);

    // P0 (SMAP): waitpid() is a kernel function that writes a kernel-side out
    // param; only this syscall boundary crosses into user memory, via put_user.
    int  kstatus = 0;
    auto result  = cinux::proc::waitpid(pid, &kstatus, options, cinux::proc::g_pid_alloc);

    if (result == cinux::proc::WaitpidResult::Ok) {
        if (status != nullptr && !cinux::user::put_user(kstatus, status)) {
            return -cinux::kEfault;  // child already reaped; status write failed
        }
        return static_cast<int64_t>(pid);
    }

    if (result == cinux::proc::WaitpidResult::NotExited) {
        // Child exists but has not exited yet; return 0 (non-blocking)
        return 0;
    }

    // Error: return negated errno
    return static_cast<int64_t>(result);
}

}  // namespace cinux::syscall
