/**
 * @file    scoped_guard.hpp
 * @brief   One RAII shell, every enter/exit discipline.
 *
 * The pattern repeats across the kernel: disable something, run, put
 * it back — preemption off and on, interrupts saved and restored,
 * locks taken and released. What varies is the pair of actions and
 * whether entering needs to remember state (the interrupt flavour
 * must, so a nested exit can restore the flags the nest was entered
 * under, not blindly re-enable). A policy supplies those three
 * things: a State it reads on the way in, Enter, and Exit; the shell
 * is the same five lines every time. Policies live in their own
 * homes — preemption in proc, interrupt flags in arch, locks in the
 * sync station — so this header stays OS-agnostic and host-testable
 * with toy policies.
 *
 * @author  Charliechen114514
 * @date    2026-10-07
 * @version 0.1
 * @since   0.1.0
 * @ingroup base_utility
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <concepts>

namespace cinux::base {

/**
 * @brief         What a guard policy must offer the shell.
 * @tparam        Policy   The discipline being wrapped.
 * @note          State is the policy's scratch slot; empty for
 *                disciplines that need no memory (a counter flip),
 *                loaded by Enter for ones that must restore (the
 *                interrupt flag). The shell default-initializes it
 *                before Enter runs.
 * @since         0.1.0
 * @ingroup       base_utility
 */
template <typename Policy>
concept GuardPolicy = requires(Policy::State& state) {
    typename Policy::State;
    { Policy::enter(state) } -> std::same_as<void>;
    { Policy::exit(state) } -> std::same_as<void>;
};

/**
 * @brief         RAII shell around one policy's enter/exit pair.
 * @tparam        Policy   The discipline this guard enforces.
 * @note          Copying a guard would double-exit, so copies and
 *                moves are deleted; scopes nest by construction.
 * @since         0.1.0
 * @ingroup       base_utility
 */
template <GuardPolicy Policy>
class ScopedGuard {
public:
    ScopedGuard() { Policy::enter(state_); }

    ~ScopedGuard() { Policy::exit(state_); }

    ScopedGuard(const ScopedGuard&)            = delete;
    ScopedGuard& operator=(const ScopedGuard&) = delete;
    ScopedGuard(ScopedGuard&&)                 = delete;
    ScopedGuard& operator=(ScopedGuard&&)      = delete;

private:
    Policy::State state_{};
};

}  // namespace cinux::base
