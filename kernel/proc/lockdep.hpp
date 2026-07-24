/**
 * @file kernel/proc/lockdep.hpp
 * @brief Runtime lock-order deadlock detector (F4-M5 R6-Part2, opt-in CINUX_LOCKDEP)
 *
 * When CINUX_LOCKDEP is ON, Spinlock::acquire/release call into lockdep to:
 *   - track the stack of spinlocks held on THIS CPU (per-CPU -- the Part1
 *     global depth counter was SMP-unsafe: two CPUs clobbered one counter);
 *   - maintain a global lock-order graph (edge X->Y = "X held when Y acquired");
 *   - on each acquire, DFS the graph for a cycle (AB-BA) and kpanic if found.
 *
 * It also backs the Scheduler::schedule() "no spinlock held across a context
 * switch" assert via lockdep_held_depth().
 *
 * Compiled out (zero cost) when CINUX_LOCKDEP is off.
 *
 * Namespace: cinux::proc.  Lock identity is the Spinlock object address (static
 * Spinlocks have stable addresses; stack/temporary Spinlocks are not tracked
 * meaningfully, which is fine -- they are not lock-order hazards).
 */

#pragma once

#include <stdint.h>

namespace cinux::proc {

#ifdef CINUX_LOCKDEP

/// Call AFTER a Spinlock is acquired.  Records it on this CPU's held stack,
/// checks for recursive acquire and lock-order cycles, and adds graph edges
/// from every other held lock to this one.
void lockdep_acquired(const void* lock);

/// Call BEFORE a Spinlock is released.  Pops it from this CPU's held stack.
void lockdep_releasing(const void* lock);

/// Count of spinlocks currently held on THIS CPU (0 outside any acquire).
/// Replaces the Part1 global g_lockdep_held_depth; used by schedule()'s
/// "no spinlock across context switch" assert.
uint32_t lockdep_held_depth();

/// True iff @p lock is currently held on THIS CPU (present on this CPU's
/// held stack).  Backs the lockdep_assert_held() macro (race_detect.hpp) --
/// a "must hold this lock to touch this state" check.  O(depth), depth < 3.
bool lockdep_is_held(const void* lock);

#else

inline void     lockdep_acquired(const void* /*lock*/) {}
inline void     lockdep_releasing(const void* /*lock*/) {}
inline uint32_t lockdep_held_depth() {
    return 0;
}

#endif

}  // namespace cinux::proc
