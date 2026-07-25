/**
 * @file kernel/proc/process_group.cpp
 * @brief Process-group / session identity operations (F3-M3 batch 2)
 *
 * Pure field operations on Task's pgid / sid / session_leader / controlling_tty.
 * No scheduler or registry interaction here -- killpg (the broadcast side)
 * lives in signal.cpp because it walks the pid registry.
 */

#include "kernel/proc/process_group.hpp"

#include "kernel/proc/process.hpp"

namespace cinux::proc {

int setpgid(Task* task, int pgid) {
    if (task == nullptr) {
        return -3;  // ESRCH
    }
    if (pgid < 0) {
        return -22;  // EINVAL
    }
    if (pgid == 0) {
        // POSIX: pgid 0 means "lead a group whose id is my own pid".
        pgid = task->pid;
    }
    // MVP: no permission / exec / cross-session checks (single-user root model).
    task->pgid = pgid;
    return 0;
}

int getpgid(const Task* task) {
    return (task != nullptr) ? task->pgid : -3;  // ESRCH
}

int getsid(const Task* task) {
    return (task != nullptr) ? task->sid : -3;  // ESRCH
}

int setsid(Task* task) {
    if (task == nullptr) {
        return -3;  // ESRCH
    }
    // Linux: a process that already leads a process group cannot call setsid.
    if (task->pgid == task->pid) {
        return -1;  // EPERM
    }
    task->pgid            = task->pid;
    task->sid             = task->pid;
    task->session_leader  = task;
    task->controlling_tty = -1;  // a new session has no controlling terminal
    return task->pid;            // success: the return value is the new sid
}

}  // namespace cinux::proc
