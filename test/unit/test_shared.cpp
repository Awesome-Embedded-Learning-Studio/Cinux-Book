#include "../framework/framework.hpp"
#include "cinux/ref_count.hpp"
#include "cinux/shared.hpp"
#include "test_assert.hpp"

namespace {

struct Node : cinux::base::RefCount<> {
    cinux::base::WeakAnchor anchor;
    unsigned                value = 7;
};

void burst_copies_and_drain(const cinux::base::Shared<Node>& first) {
    {
        // NOLINTNEXTLINE(performance-unnecessary-copy-initialization, misc-const-correctness)
        auto second = first;
        ASSERT_TRUE(first->use_count() == 2);
        // NOLINTNEXTLINE(performance-unnecessary-copy-initialization, misc-const-correctness)
        auto third = second;
        ASSERT_TRUE(third.holds());
        ASSERT_TRUE(third->use_count() == 3);
        ASSERT_TRUE(first->use_count() == 3);
    }
    ASSERT_TRUE(first->use_count() == 1);
}

void write_through_and_release(cinux::base::Shared<Node>& first) {
    first->value = 9;
    ASSERT_TRUE(first->value == 9);
    first.reset();
    ASSERT_TRUE(!first.holds());
}

TEST("shared: copies retain and the last release deletes") {
    auto first = cinux::base::MakeShared<Node>();
    ASSERT_TRUE(first.holds());
    ASSERT_TRUE(first->use_count() == 1);
    burst_copies_and_drain(first);
    write_through_and_release(first);
}

TEST("shared: move hands the hold off") {
    auto first = cinux::base::MakeShared<Node>();
    auto moved = static_cast<cinux::base::Shared<Node>&&>(first);
    // NOLINTNEXTLINE(clang-analyzer-cplusplus.Move) the drained sender is the point
    ASSERT_TRUE(!first.holds());
    ASSERT_TRUE(moved.holds());
    ASSERT_TRUE(moved->use_count() == 1);
    auto reassigned = cinux::base::MakeShared<Node>();
    reassigned      = static_cast<cinux::base::Shared<Node>&&>(moved);
    // NOLINTNEXTLINE(clang-analyzer-cplusplus.Move) the drained sender is the point
    ASSERT_TRUE(!moved.holds());
    ASSERT_TRUE(reassigned->use_count() == 1);
}

TEST("weak: lock answers shared while the object lives") {
    const auto              kHeld = cinux::base::MakeShared<Node>();
    // NOLINTNEXTLINE(misc-const-correctness) watcher mutates on assignment paths
    cinux::base::Weak<Node> watcher(kHeld->anchor, kHeld.get());
    ASSERT_TRUE(watcher.watching());
    const auto kLocked = watcher.lock();
    ASSERT_TRUE(kLocked.holds());
    ASSERT_TRUE(kHeld->use_count() == 2);
}

TEST("weak: the dying object empties its watchers") {
    cinux::base::Weak<Node> watcher;
    {
        const auto kHeld = cinux::base::MakeShared<Node>();
        watcher          = cinux::base::Weak<Node>(kHeld->anchor, kHeld.get());
        ASSERT_TRUE(watcher.watching());
    }
    ASSERT_TRUE(!watcher.watching());
    const auto kLocked = watcher.lock();
    ASSERT_TRUE(!kLocked.holds());
}

TEST("weak: watcher destruction leaves the object alone") {
    auto held = cinux::base::MakeShared<Node>();
    {
        const cinux::base::Weak<Node> kWatcher(held->anchor, held.get());
    }
    ASSERT_TRUE(held->use_count() == 1);
    held.reset();
}

TEST("weak: many watchers all vacate together") {
    cinux::base::Weak<Node> first;
    cinux::base::Weak<Node> second;
    cinux::base::Weak<Node> third;
    {
        const auto kHeld = cinux::base::MakeShared<Node>();
        first            = cinux::base::Weak<Node>(kHeld->anchor, kHeld.get());
        second           = cinux::base::Weak<Node>(kHeld->anchor, kHeld.get());
        third            = cinux::base::Weak<Node>(kHeld->anchor, kHeld.get());
    }
    ASSERT_TRUE(!first.watching());
    ASSERT_TRUE(!second.watching());
    ASSERT_TRUE(!third.watching());
}

}  // namespace

int main() {
    return cinux::test::RunAll();
}
