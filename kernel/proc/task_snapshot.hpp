/**
 * @file    task_snapshot.hpp
 * @brief   One task's vital signs, copied out under the registry's lock.
 *
 * A pointer to a task is a promise someone is keeping: between handing
 * it out and reading its fields, the task may exit and be freed, and a
 * reader holding the bare pointer would walk freed memory. The answer
 * is not to hold tasks alive — it is to not hold tasks at all: copy the
 * wanted fields while the registry's lock still covers the walk, and
 * let the snapshot, a plain bag of values with no pointer in it, travel
 * anywhere afterwards. The name buffer is bytes, not a pointer, on the
 * same discipline: whatever tomorrow changes about how names are
 * stored, the snapshot stays a copy.
 *
 * @author  Charliechen114514
 * @date    2026-10-19
 * @version 0.1
 * @since   0.5.0
 * @ingroup kernel_proc
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <cstdint>

namespace cinux::proc {

/// One snapshot's name budget, NUL included, the hosted comm length.
inline constexpr unsigned long kTaskNameMax = 16;

/**
 * @brief   One task's vital signs, safe to carry outside any lock.
 * @note    Born empty; filled field by field by whoever holds the
 *          registry lock. Every field is a value — nothing here points
 *          back at the task it came from.
 * @since   0.5.0
 * @ingroup kernel_proc
 */
struct TaskSnapshot {
    /// The task's own pid.
    long pid = 0;

    /// The parent's pid, zero at the root of the tree.
    long parent_pid = 0;

    /// The process group the task leads or rides.
    long pgid = 0;

    /// The session the task belongs to.
    long sid = 0;

    /// The scheduler's one-letter state, ps-shaped.
    char state = 'R';

    /// The name, NUL-terminated bytes.
    char name[kTaskNameMax] = {};
};

}  // namespace cinux::proc
