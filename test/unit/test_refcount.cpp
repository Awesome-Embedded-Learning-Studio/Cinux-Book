/**
 * @file test/unit/test_refcount.cpp
 * @brief Unit tests for cinux::lib::RefCount (saturating atomic refcount).
 *
 * Covers the core lifetime and saturation contract that Q4b-e consumers
 * (SharedCwd / FDTable / AddressSpace / Task) will rely on: acquire bumps,
 * release returns true only at the 0 transition, and underflow / overflow /
 * saturated state all clamp to kRefcountSaturated instead of wrapping (the UAF
 * defence that justifies RefCount over a plain atomic).
 *
 * Header-only Cinux-Base type; covered by the host test build because the
 * submodule's own test/ is never built by the main repo (third_party/
 * CMakeLists.txt does not add_subdirectory(Cinux-Base)).
 *
 * Compile condition: -DCINUX_HOST_TEST
 */

#define TEST_FRAMEWORK_IMPL
#include <cinux/refcount.hpp>

#include "test_framework.h"

using cinux::lib::RefCount;
using cinux::lib::kRefcountSaturated;

// ============================================================
// Basic lifetime
// ============================================================

TEST("refcount: fresh counter starts at 1") {
    RefCount rc;
    ASSERT_EQ(rc.load(), 1u);
}

TEST("refcount: explicit initial count") {
    RefCount rc(3);
    ASSERT_EQ(rc.load(), 3u);
}

TEST("refcount: acquire bumps the count") {
    RefCount rc;
    rc.acquire();
    rc.acquire();
    ASSERT_EQ(rc.load(), 3u);
}

TEST("refcount: release returns false above zero") {
    RefCount rc(2);
    ASSERT_FALSE(rc.release());  // 2 -> 1
    ASSERT_EQ(rc.load(), 1u);
}

TEST("refcount: release returns true at the zero transition") {
    RefCount rc(1);
    ASSERT_TRUE(rc.release());  // 1 -> 0, last reference gone
    ASSERT_EQ(rc.load(), 0u);
}

TEST("refcount: balanced acquire/release round-trip") {
    RefCount rc;
    rc.acquire();                // 2
    rc.acquire();                // 3
    ASSERT_FALSE(rc.release());  // 3 -> 2
    ASSERT_FALSE(rc.release());  // 2 -> 1
    ASSERT_TRUE(rc.release());   // 1 -> 0 (caller frees)
    ASSERT_EQ(rc.load(), 0u);
}

// ============================================================
// Saturation / UAF defence — the reason RefCount exists over a plain atomic.
// A plain atomic would wrap on extra release/overflow; RefCount clamps.
// ============================================================

TEST("refcount: extra release after zero saturates (no wraparound UAF)") {
    RefCount rc(1);
    ASSERT_TRUE(rc.release());   // 1 -> 0
    ASSERT_FALSE(rc.release());  // underflow -> clamp saturated
    ASSERT_EQ(rc.load(), kRefcountSaturated);
}

TEST("refcount: saturated counter is sticky under further release") {
    RefCount rc(1);
    ASSERT_TRUE(rc.release());   // -> 0
    ASSERT_FALSE(rc.release());  // saturate
    ASSERT_FALSE(rc.release());  // stays saturated
    ASSERT_FALSE(rc.release());  // stays saturated
    ASSERT_EQ(rc.load(), kRefcountSaturated);
}

TEST("refcount: acquire on a saturated counter does not bump") {
    RefCount rc(1);
    ASSERT_TRUE(rc.release());   // -> 0
    ASSERT_FALSE(rc.release());  // saturate
    rc.acquire();                // must NOT bump above saturated
    ASSERT_EQ(rc.load(), kRefcountSaturated);
    rc.acquire();
    ASSERT_EQ(rc.load(), kRefcountSaturated);
}

TEST("refcount: explicitly saturated construction is sticky") {
    RefCount rc(kRefcountSaturated);
    rc.acquire();
    ASSERT_EQ(rc.load(), kRefcountSaturated);
    ASSERT_FALSE(rc.release());
    ASSERT_EQ(rc.load(), kRefcountSaturated);
}

// ============================================================
// Main
// ============================================================

int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}
