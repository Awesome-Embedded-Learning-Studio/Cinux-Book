#include <cstdint>

#include "../framework/framework.hpp"
#include "cinux/random.hpp"
#include "test_assert.hpp"

namespace {

using cinux::base::random::Generator;

}  // namespace

TEST("random: one seed, two generators, the same stream") {
    Generator left{};
    Generator right{};
    cinux::base::random::Seed(left, 20261023ULL);
    cinux::base::random::Seed(right, 20261023ULL);
    for (unsigned draw = 0; draw < 8; ++draw) {
        ASSERT_EQ(cinux::base::random::Next(left), cinux::base::random::Next(right));
    }
}

TEST("random: different seeds drift apart right away") {
    Generator left{};
    Generator right{};
    cinux::base::random::Seed(left, 1ULL);
    cinux::base::random::Seed(right, 2ULL);
    bool apart = false;
    for (unsigned draw = 0; draw < 8; ++draw) {
        if (cinux::base::random::Next(left) != cinux::base::random::Next(right)) {
            apart = true;
        }
    }
    ASSERT_TRUE(apart);
}

TEST("random: a seeded stream is neither zero nor stuck") {
    Generator generator{};
    cinux::base::random::Seed(generator, 42ULL);
    unsigned long long nonzero  = 0;
    std::uint64_t      previous = 0;
    bool               moving   = false;
    for (unsigned draw = 0; draw < 16; ++draw) {
        const std::uint64_t kWord = cinux::base::random::Next(generator);
        nonzero += kWord != 0 ? 1 : 0;
        moving   = moving || kWord != previous;
        previous = kWord;
    }
    ASSERT_TRUE(nonzero > 14);
    ASSERT_TRUE(moving);
}

TEST("random: the low bits visit both parities") {
    Generator generator{};
    cinux::base::random::Seed(generator, 114514ULL);
    unsigned even = 0;
    for (unsigned draw = 0; draw < 64; ++draw) {
        even += (cinux::base::random::Next(generator) & 1ULL) == 0 ? 1 : 0;
    }
    ASSERT_TRUE(even > 16);
    ASSERT_TRUE(even < 48);
}

int main() {
    return cinux::test::RunAll();
}
