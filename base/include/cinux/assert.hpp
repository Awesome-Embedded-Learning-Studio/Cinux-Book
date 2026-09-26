/**
 * @file    assert.hpp
 * @brief   Assertion hook shared by the host and kernel worlds.
 *
 * The declaration lives here; each world links its own definition at build
 * time — the host side prints to stderr and aborts (test/framework/
 * assert_host.cpp), the kernel side routes the failure into panic
 * (station 03). Call sites never change when the world changes.
 *
 * @author  Charliechen114514
 * @date    2026-09-25
 * @version 0.1
 * @since   0.1.0
 * @ingroup base_safety
 */

#pragma once

#include <source_location>

namespace cinux::base::safety {

/**
 * @brief         Terminates the process with a diagnostic report.
 *
 * @param[in]     k_message   Description of the violated condition.
 * @param[in]     loc         Capture point of the failed check; defaults
 *                            to the caller's location.
 * @return        None
 * @note          Marked [[noreturn]]. Defined once per world and selected
 *                at link time; the host definition prints to stderr
 *                (unbuffered, so output survives abort) and aborts. The
 *                kernel definition routes the failure into panic at
 *                station 03.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       base_safety
 */
[[noreturn]] void AssertionFailed(const char*          k_message,
                                  std::source_location loc = std::source_location::current());

/**
 * @brief         Checks a condition and terminates when it does not hold.
 *
 * @param[in]     condition_result   Condition to verify.
 * @param[in]     k_message          Description used in the failure report.
 * @param[in]     loc                Capture point; defaults to the caller.
 * @return        None
 * @note          Inline on purpose: a single forwarding branch, kept at the
 *                call site so the default source_location argument captures
 *                the actual checking location.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       base_safety
 */
inline void Check(bool condition_result, const char* k_message,
                  std::source_location loc = std::source_location::current()) {
    if (condition_result) {
        return;
    }

    AssertionFailed(k_message, loc);
}

}  // namespace cinux::base::safety
