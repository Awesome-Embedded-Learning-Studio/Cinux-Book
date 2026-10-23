/**
 * @file    entropy.cpp
 * @brief   The seeding itself: hardware draws, cycles, one address.
 *
 * @author  Charliechen114514
 * @date    2026-10-23
 * @version 0.3.0
 * @since   0.3.0
 * @ingroup kernel_arch
 * @copyright Copyright (c) 2026
 */

#include "kernel/arch/x86_64/entropy.hpp"

#include "cinux/random.hpp"
#include "kernel/arch/x86_64/rdrand.hpp"

namespace cinux::arch {

namespace {

/// The one generator every layout consumer shares.
base::random::Generator g_generator{};

/// The kernel's own virtual landing address, mixed in because a
/// compiler-chosen seat is still a fact two boots rarely share.
extern "C" unsigned char __vma_start[];

/// The cycle counter; free-running since reset and different on every
/// boot by construction.
unsigned long long cycle_count() {
    unsigned int low  = 0;
    unsigned int high = 0;
    asm volatile("rdtsc" : "=a"(low), "=d"(high));
    return (static_cast<unsigned long long>(high) << 32U) | static_cast<unsigned long long>(low);
}

}  // namespace

void SeedKernelRandom() {
    auto seed = reinterpret_cast<unsigned long long>(__vma_start);
    seed ^= cycle_count();
    unsigned long long draw = 0;
    if (TryRdrand(draw)) {
        seed ^= draw << 17U;
    }
    if (TryRdrand(draw)) {
        seed ^= draw << 31U;
    }
    base::random::Seed(g_generator, seed);
}

unsigned long long RandomWord() {
    return base::random::Next(g_generator);
}

}  // namespace cinux::arch
