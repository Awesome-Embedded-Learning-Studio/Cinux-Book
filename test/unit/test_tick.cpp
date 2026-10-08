#include <thread>

#include "cinux/literal_types.hpp"
#include "framework.hpp"
#include "kernel/time/tick.hpp"
#include "kernel/time/tick_config.hpp"
#include "test_assert.hpp"

namespace {

class FakeBackend {
public:
    void start(cinux::base::Hertz rate) {
        started = rate;
        ++starts;
    }

    cinux::base::Hertz started{};
    unsigned int       starts = 0;
};

TEST("tick: init hands the configured rate to the backend") {
    FakeBackend backend;
    cinux::time::Tick::self().init(backend);
    ASSERT_EQ(backend.started.value, cinux::time::kTickHz.value);
    ASSERT_EQ(backend.starts, 1U);
}

TEST("tick: init resets the counter and later inits restart cleanly") {
    cinux::time::Tick::self().on_interrupt();
    cinux::time::Tick::self().on_interrupt();
    ASSERT_EQ(cinux::time::Tick::self().since_boot(), 2ULL);

    FakeBackend backend;
    cinux::time::Tick::self().init(backend);
    ASSERT_EQ(cinux::time::Tick::self().since_boot(), 0ULL);
}

TEST("tick: every interrupt advances the boot counter") {
    FakeBackend backend;
    cinux::time::Tick::self().init(backend);

    for (unsigned int beat = 0; beat < 3; ++beat) {
        cinux::time::Tick::self().on_interrupt();
    }
    ASSERT_EQ(cinux::time::Tick::self().since_boot(), 3ULL);
}

TEST("tick: concurrent interrupt producers and reader share an atomic counter") {
    FakeBackend backend;
    auto&       tick = cinux::time::Tick::self();
    tick.init(backend);
    constexpr unsigned int kBeats   = 10000;
    auto                   producer = [&tick] {
        for (unsigned int beat = 0; beat < kBeats; ++beat) {
            tick.on_interrupt();
        }
    };
    std::thread        first(producer);
    std::thread        second(producer);
    unsigned long long previous  = 0;
    bool               monotonic = true;
    for (unsigned int sample = 0; sample < kBeats; ++sample) {
        const auto kNow = tick.since_boot();
        monotonic       = monotonic && kNow >= previous;
        previous        = kNow;
    }
    first.join();
    second.join();
    ASSERT_TRUE(monotonic);
    ASSERT_EQ(tick.since_boot(), 2ULL * kBeats);
}

}  // namespace

int main() {
    return cinux::test::RunAll();
}
