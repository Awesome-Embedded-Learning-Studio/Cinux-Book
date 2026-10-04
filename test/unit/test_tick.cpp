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

}  // namespace

int main() {
    return cinux::test::RunAll();
}
