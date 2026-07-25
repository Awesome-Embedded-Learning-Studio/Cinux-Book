/**
 * @file kernel/proc/process_group.hpp
 * @brief Process-group / session identity operations (F3-M3 batch 2)
 *
 * setpgid / getpgid / getsid / setsid -- the identity side of POSIX job
 * control.  The signal-broadcast side (killpg) lives in signal.hpp because it
 * walks the pid registry.  Controlling-terminal attach is deferred to F10-M3
 * (the TTY milestone); here a new session just clears controlling_tty.
 *
 * Namespace: cinux::proc
 */

#pragma once

namespace cinux::proc {

struct Task;  // full definition in process.hpp

/**
 * @brief Set @p task's process-group ID to @p pgid
 *
 * @p pgid == 0 means "use the task's own pid" -- the task leads a fresh group.
 * Returns 0 on success, or -errno.
 *
 * MVP scope (single-user / root model): omits Linux's EACCES (changing a task
 * that has already exec'd), EPERM (moving into a group in another session),
 * and the "only self or a direct child" restriction.
 */
int setpgid(Task* task, int pgid);

/// @brief Query @p task's process-group ID (>= 0), or -errno.
int getpgid(const Task* task);

/// @brief Query @p task's session ID (>= 0), or -errno.
int getsid(const Task* task);

/**
 * @brief Create a new session and process group; @p task becomes the leader
 *
 * The task's pgid and sid both become its pid, it points session_leader at
 * itself, and its controlling terminal is cleared.  Returns the new sid
 * (> 0), or -errno (EPERM if the task already leads a process group).
 */
int setsid(Task* task);

}  // namespace cinux::proc
