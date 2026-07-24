/**
 * @file kernel/proc/race_detect.hpp
 * @brief SMP data-race watchpoint detector (F-DYN-COV, opt-in CINUX_RACE_DETECT)
 *
 * Two complementary tools for hunting lockless shared-state races:
 *
 * 1. lockdep_assert_held(lock) -- "this access requires @p lock held".  Backed
 *    by LOCKDEP's per-CPU held stack (lockdep_is_held); catches "designed a
 *    lock but forgot to take it on some path" -- a regression guard for
 *    already-fixed races (NVMe io_lock_, buddy free lock, ext2 block_buf_
 *    reloads).  No-op when CINUX_LOCKDEP is off.
 *
 * 2. RACE_TOUCH(w) -- "this shared variable ought to be lock-protected".  A
 *    RaceWatchpoint records the last CPU that touched it; if a DIFFERENT CPU
 *    touches it next, that is a cross-CPU access with no lock in between --
 *    the signature of a lockless shared-state race (e.g. ext2 inode_cache_
 *    before it got a lock).  kpanic with the offender stack.  No-op when
 *    CINUX_RACE_DETECT is off.
 *
 * The watchpoint reports any cross-CPU interleaving, even a serial one.  That
 * is intentional: in a hobby kernel every shared mutable variable SHOULD
 * carry a lock, so "touched from two CPUs with no lock" is itself the design
 * defect to surface -- it forces adding a lock rather than relying on timing.
 *
 * race_check_access_probe() returns bool without panicking, so the mechanism
 * test can verify the detection logic without halting the kernel.
 *
 * Namespace: cinux::proc.
 */

#pragma once

#include <stdint.h>

#include "kernel/lib/kprintf.hpp"   // kpanic (lockdep_assert_held macro)
#include "kernel/proc/lockdep.hpp"  // lockdep_is_held (lockdep_assert_held macro)

namespace cinux::proc {

/// Sentinel for "no CPU has touched this watchpoint yet".
constexpr uint32_t kRaceCpuNone = 0xFFFFFFFFu;

/// A watchpoint over one shared variable.  Initialise with RACE_WATCHPOINT_INIT.
/// `last_cpu` is kRaceCpuNone until first touched, then the CPU id of the most
/// recent toucher.  Touched only via __atomic_exchange_n (ACQ_REL).
struct RaceWatchpoint {
    const char*       name;
    volatile uint32_t last_cpu;
};

#define RACE_WATCHPOINT_INIT(n) { (n), cinux::proc::kRaceCpuNone }

/// Probe a watchpoint: record THIS CPU as the last toucher and report whether
/// a DIFFERENT CPU touched it since this CPU's last access.  Never panics --
/// the mechanism test uses this to verify detection.  Returns true iff a
/// cross-CPU interleaving was observed (i.e. a suspected race).
bool race_check_access_probe(RaceWatchpoint& w);

/// RACE_TOUCH entry: probe and kpanic with the offender stack on a cross-CPU
/// interleaving.  Reach race_check_access_probe directly only from tests; use
/// the RACE_TOUCH macro at access sites.
void race_check_access(RaceWatchpoint& w);

}  // namespace cinux::proc

// lockdep_assert_held is a macro so callers do not need #ifdef at every site:
// it compiles to a no-op when CINUX_LOCKDEP is off.  It depends on
// lockdep_is_held (declared in lockdep.hpp, included above).
#ifdef CINUX_LOCKDEP
#    define lockdep_assert_held(lock)                                        \
        do {                                                                 \
            if (!cinux::proc::lockdep_is_held((lock))) {                     \
                cinux::lib::kpanic("lockdep: assert_held failed %p",         \
                                   static_cast<const void*>(lock));          \
            }                                                                \
        } while (0)
#else
#    define lockdep_assert_held(lock) ((void)0)
#endif

// RACE_TOUCH is a macro for the same reason: no-op when CINUX_RACE_DETECT is
// off, so access sites need no #ifdef.
#ifdef CINUX_RACE_DETECT
#    define RACE_TOUCH(w) cinux::proc::race_check_access((w))
#else
#    define RACE_TOUCH(w) ((void)0)
#endif
