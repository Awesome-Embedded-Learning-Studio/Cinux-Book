/**
 * @file    random.hpp
 * @brief   One pseudorandom generator, seeded once and never trusted.
 *
 * xoshiro256** over a four-word state: the algorithm is small, its
 * period is long, and its arithmetic is plain enough for every world
 * this tree compiles for. The generator is good enough for layout
 * jitter and cookies — the kind of randomness that wants no two boots
 * to agree, not the kind that faces an adversary who studies the
 * output. Callers with adversaries bring their own hardware source.
 *
 * @author  Charliechen114514
 * @date    2026-10-23
 * @version 0.3.0
 * @since   0.3.0
 * @ingroup base_random
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <stdint.h>

namespace cinux::base::random {

/// The generator's whole state; four words, zero means never seeded.
struct Generator {
    /// State words; zeroed until a seed arrives.
    uint64_t state[4]{};
};

/**
 * @brief         Mixes one word into a seeding scramble.
 *
 * @param[in,out] value   The running seed; mixed in place.
 * @return        The scrambled word.
 * @since         0.3.0
 * @ingroup       base_random
 */
inline uint64_t SplitMix64(uint64_t& value) {
    value += 0x9E37'79B9'7F4A'7C15ULL;
    uint64_t mixed = value;
    mixed          = (mixed ^ (mixed >> 30U)) * 0xBF58'476D'1CE4'E5B9ULL;
    mixed          = (mixed ^ (mixed >> 27U)) * 0x94D0'49BB'1331'11EBULL;
    return mixed ^ (mixed >> 31U);
}

/**
 * @brief         Seeds a generator from one word.
 *
 * @param[out]    generator   The generator to arm.
 * @param[in]     seed        Any word; four scrambles spread it over
 *                            the state.
 * @return        None.
 * @since         0.3.0
 * @ingroup       base_random
 */
inline void Seed(Generator& generator, uint64_t seed) {
    for (uint64_t& word : generator.state) {
        word = SplitMix64(seed);
    }
}

/**
 * @brief         Draws the next word.
 *
 * @param[in,out] generator   The armed generator; the state advances.
 * @return        The next pseudorandom word.
 * @note          Advancing an unseeded generator stays unseeded in
 *                spirit — all zeros in, all zeros out — so callers
 *                that forget to seed get boring answers, not traps.
 * @since         0.3.0
 * @ingroup       base_random
 */
inline uint64_t Next(Generator& generator) {
    const uint64_t kSpun   = generator.state[1] * 5U;
    const uint64_t kResult = ((kSpun << 7U) | (kSpun >> 57U)) * 9U;
    const uint64_t kTail   = generator.state[1] << 17U;
    generator.state[2] ^= generator.state[0];
    generator.state[3] ^= generator.state[1];
    generator.state[1] ^= generator.state[2];
    generator.state[0] ^= generator.state[3];
    generator.state[2] ^= kTail;
    generator.state[3] = (generator.state[3] << 45U) | (generator.state[3] >> 19U);
    return kResult;
}

}  // namespace cinux::base::random
