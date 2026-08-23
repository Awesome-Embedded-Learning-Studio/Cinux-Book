/**
 * @file random.hpp
 * @brief xoshiro256** pseudo-random number generator — no heap, no external entropy.
 */

#ifndef CINUX_RANDOM_HPP
#define CINUX_RANDOM_HPP

#include <array>
#include <cstddef>
#include <cstdint>

namespace cinux::lib {

/**
 * @brief Deterministic PRNG based on xoshiro256**.
 *
 * Not cryptographically secure. Seed from hardware entropy in kernel.
 */
class StaticRandomSource {
public:
    /** @brief Seed the generator using SplitMix64 expansion. */
    constexpr void seed(uint64_t seed_value) {
        for (int i = 0; i < 4; ++i) {
            seed_value += 0x9E3779B97F4A7C15ULL;
            uint64_t z = seed_value;
            z          = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
            z          = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
            state_[i]  = z ^ (z >> 31);
        }
        seeded_ = true;
    }

    /** @brief Generate next 64-bit random value. */
    constexpr uint64_t next_u64() {
        uint64_t* s      = state_.data();
        uint64_t  result = rotl(s[1] * 5, 7) * 9;
        uint64_t  t      = s[1] << 17;

        s[2] ^= s[0];
        s[3] ^= s[1];
        s[1] ^= s[2];
        s[0] ^= s[3];

        s[2] ^= t;
        s[3] = rotl(s[3], 45);

        return result;
    }

    /** @brief Generate next 32-bit random value. */
    constexpr uint32_t next_u32() { return static_cast<uint32_t>(next_u64() >> 32); }

    /** @brief Generate uniform random value in [0, max). */
    constexpr uint32_t next_bounded(uint32_t max) {
        if (max == 0) {
            return 0;
        }
        uint64_t bound = static_cast<uint64_t>(max);
        uint64_t val   = (static_cast<uint64_t>(next_u32()) * bound) >> 32;
        return static_cast<uint32_t>(val);
    }

    /** @brief Fill a buffer with random bytes. */
    void fill(void* buf, size_t len) {
        auto*  dst = static_cast<uint8_t*>(buf);
        size_t i   = 0;

        while (i + 8 <= len) {
            uint64_t v = next_u64();
            for (int j = 0; j < 8; ++j) {
                dst[i++] = static_cast<uint8_t>(v & 0xFF);
                v >>= 8;
            }
        }

        if (i < len) {
            uint64_t v = next_u64();
            while (i < len) {
                dst[i++] = static_cast<uint8_t>(v & 0xFF);
                v >>= 8;
            }
        }
    }

    /** @brief Whether the generator has been seeded. */
    constexpr bool seeded() const { return seeded_; }

private:
    static constexpr uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

    std::array<uint64_t, 4> state_{};
    bool                    seeded_ = false;
};

}  // namespace cinux::lib

#endif  // CINUX_RANDOM_HPP
