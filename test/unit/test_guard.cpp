#include "../framework/framework.hpp"
#include "cinux/scoped_guard.hpp"
#include "test_assert.hpp"

namespace {

struct ToyPolicy {
    using State = int;

    static void enter(State& state) {
        g_events[g_len++] = '+';
        state             = 7;
    }

    static void exit(State& state) {
        ASSERT_TRUE(state == 7);
        g_events[g_len++] = '-';
    }

    static char g_events[8];
    static int  g_len;
};

char ToyPolicy::g_events[8] = {};
int  ToyPolicy::g_len       = 0;

struct CounterPolicy {
    using State = struct {};

    static void enter(State& /*state*/) { g_depth++; }

    static void exit(State& /*state*/) { g_depth--; }

    static int g_depth;
};

int CounterPolicy::g_depth = 0;

TEST("guard: policy hooks run in scope order") {
    ToyPolicy::g_len = 0;
    {
        const cinux::base::ScopedGuard<ToyPolicy> kGuard;
        ASSERT_TRUE(ToyPolicy::g_len == 1);
        ASSERT_TRUE(ToyPolicy::g_events[0] == '+');
    }
    ASSERT_TRUE(ToyPolicy::g_len == 2);
    ASSERT_TRUE(ToyPolicy::g_events[1] == '-');
}

TEST("guard: nesting unwinds inside-out") {
    ToyPolicy::g_len = 0;
    {
        const cinux::base::ScopedGuard<ToyPolicy> kOuter;
        {
            const cinux::base::ScopedGuard<ToyPolicy> kInner;
            ASSERT_TRUE(ToyPolicy::g_len == 2);
        }
        ASSERT_TRUE(ToyPolicy::g_len == 3);
        ASSERT_TRUE(ToyPolicy::g_events[2] == '-');
    }
    ASSERT_TRUE(ToyPolicy::g_len == 4);
    ASSERT_TRUE(ToyPolicy::g_events[3] == '-');
}

TEST("guard: empty-state policies compile and count") {
    CounterPolicy::g_depth = 0;
    {
        const cinux::base::ScopedGuard<CounterPolicy> kOuter;
        {
            const cinux::base::ScopedGuard<CounterPolicy> kInner;
            ASSERT_TRUE(CounterPolicy::g_depth == 2);
        }
        ASSERT_TRUE(CounterPolicy::g_depth == 1);
    }
    ASSERT_TRUE(CounterPolicy::g_depth == 0);
}

}  // namespace

int main() {
    return cinux::test::RunAll();
}
