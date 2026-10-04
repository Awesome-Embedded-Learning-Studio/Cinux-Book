/**
 * @file    test_case.hpp
 * @brief   The one test-case record shared by both worlds.
 *
 * Host and kernel runners walk the same record shape; only registration
 * differs — static constructors on the host, a linker-section table in the
 * kernel. Keeping the struct in a single header is what makes the twin a
 * twin: layout drift between the two worlds would fail silently.
 *
 * @author  Charliechen114514
 * @date    2026-10-02
 * @version 0.1
 * @since   0.1.0
 * @ingroup test_framework
 * @copyright Copyright (c) 2026
 */

#pragma once

namespace cinux::test {

/**
 * @brief         A single registered test case.
 * @note          Plain data by design: the kernel world stores these in a
 *                dedicated linker section where no constructor could ever
 *                run, so the record carries nothing but two pointers.
 * @since         0.1.0
 * @ingroup       test_framework
 */
struct TestCase {
    const char* name;  ///< Human-readable case name, printed by the runner.
    void (*body)();    ///< Test body, typically a lambda emitted by TEST().
};

}  // namespace cinux::test
