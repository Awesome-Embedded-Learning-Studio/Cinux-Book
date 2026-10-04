#include "framework_kernel.hpp"
#include "kernel/time/tick.hpp"
#include "test_assert.hpp"
#include "test_case.hpp"  // NOLINT(misc-include-cleaner) consumed by the TEST() macro body

namespace {

constexpr unsigned long long kHeartbeatsWanted = 3;

constexpr unsigned long long kWaitSpinCap = 2000000000ULL;

constexpr unsigned long long kQuietSpinCap = 100000000ULL;

TEST("tick: heartbeats arrive once the gates are open") {
    const unsigned long long kBefore = cinux::time::Tick::self().since_boot();
    unsigned long long       spins   = 0;
    while (cinux::time::Tick::self().since_boot() < kBefore + kHeartbeatsWanted &&
           spins < kWaitSpinCap) {
        ++spins;
    }
    ASSERT_GE(cinux::time::Tick::self().since_boot(), kBefore + kHeartbeatsWanted);
}

TEST("tick: silence while the cpu gates are closed") {
    asm volatile("cli" : : : "memory");
    const unsigned long long kFrozen = cinux::time::Tick::self().since_boot();
    for (unsigned long long spins = 0; spins < kQuietSpinCap; ++spins) {
    }
    ASSERT_EQ(cinux::time::Tick::self().since_boot(), kFrozen);
    asm volatile("sti" : : : "memory");
}

}  // namespace
