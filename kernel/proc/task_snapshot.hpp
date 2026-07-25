/**
 * @file kernel/proc/task_snapshot.hpp
 * @brief TaskSnapshot: value snapshot of a Task's /proc-relevant fields (DEBT-022)
 *
 * Split out of process.hpp (which sits at the 500-line cap) so adding the
 * snapshot type does not push it over.  TaskSnapshot holds copies of the
 * fields /proc consumes, made under the registry lock by signal_snapshot_task();
 * an unlocked Task* is a use-after-free now that tasks are freed (DEBT-002
 * fixed in F-QA Q4e-3).  name bytes are copied (not the pointer) so the
 * snapshot is self-contained regardless of name storage.
 *
 * Namespace: cinux::proc
 */

#pragma once

#include <stdint.h>

#include "kernel/proc/process.hpp"  // TaskState

namespace cinux::proc {

/// Maximum length of a task name stored in a TaskSnapshot.  NUL-terminated,
/// truncated like Linux TASK_COMM_LEN (16).
static constexpr uint32_t kTaskNameMax = 16;

/// Value snapshot of the /proc-relevant fields of a Task, copied under the
/// registry lock by signal_snapshot_task().  Holding a raw Task* outside the
/// lock is a use-after-free now that tasks are freed (DEBT-002 fixed in
/// F-QA Q4e-3); callers that only need these fields take a snapshot so they
/// never touch the Task after the lock releases.
struct TaskSnapshot {
    int       pid{};                 ///< Process ID
    TaskState state{};               ///< Lifecycle state (stat field 3)
    int       ppid{};                ///< Parent PID
    int       tgid{};                ///< Thread-group ID
    uint32_t  uid{};                 ///< Real user ID
    uint32_t  gid{};                 ///< Real group ID
    char      name[kTaskNameMax]{};  ///< NUL-terminated; bytes copied, truncated
};

}  // namespace cinux::proc
