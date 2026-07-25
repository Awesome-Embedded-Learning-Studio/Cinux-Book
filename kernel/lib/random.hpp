/**
 * @file kernel/lib/random.hpp
 * @brief KRandom — boot-seeded kernel PRNG (F9 batch 7 / M2)
 *
 * Seeded once at boot from entropy (rdrand if the CPU has it, TSC, PIT ticks,
 * and the kernel image's own load address), then a xoshiro256** stream feeds
 * ASLR (F9 batch 8) and any other kernel-side randomness.
 *
 * Honest scope: this is "good-enough" entropy for a hobby kernel's ASLR, not a
 * certified CSPRNG. The rdrand path (when available) is the strongest input;
 * TSC/PIT/address-mix are fallbacks that still defeat a naive guess.
 *
 * Namespace: cinux::lib
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace cinux::lib {

/// Boot-seeded kernel PRNG (xoshiro256**). @see @file header for scope.
class KRandom {
public:
    /// Seed the PRNG state from boot entropy. Idempotent (re-seed allowed).
    void init();

    /// Next 32 pseudo-random bits.
    uint32_t next32();

    /// Next 64 pseudo-random bits.
    uint64_t next64();

    /// Fill a buffer with pseudo-random bytes.
    void fill(void* buf, size_t len);

private:
    uint64_t state_[4]{};  ///< xoshiro256** state
    bool     initialized_ = false;
};

/// Global kernel PRNG instance. Call init() once at boot before use.
extern KRandom g_random;

}  // namespace cinux::lib
