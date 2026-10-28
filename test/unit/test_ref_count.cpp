#include <atomic>
#include <thread>
#include <type_traits>

#include "cinux/assert.hpp"
#include "cinux/ref_count.hpp"
#include "cinux/spin_mutex.hpp"
#include "framework.hpp"
#include "test/mock/ref_object.hpp"
#include "test_assert.hpp"

namespace {
using cinux::test::RefObject;
static_assert(!std::is_copy_constructible_v<RefObject>);
static_assert(!std::is_move_constructible_v<RefObject>);

TEST("ref counted: initial owner alone destroys the object at release") {
    unsigned int destroyed = 0;
    auto* const  kObject   = new RefObject(destroyed);
    ASSERT_EQ(kObject->use_count(), 1UL);
    kObject->release_ref();
    ASSERT_EQ(destroyed, 1U);
}

TEST("ref counted: separate owners keep an object alive until the last release") {
    unsigned int destroyed = 0;
    auto* const  kObject   = new RefObject(destroyed);
    kObject->add_ref();
    kObject->add_ref();
    ASSERT_EQ(kObject->use_count(), 3UL);
    kObject->release_ref();
    cinux::base::safety::Check(destroyed == 0, "first shared release destroyed a live owner");
    kObject->release_ref();
    cinux::base::safety::Check(destroyed == 0, "second shared release destroyed a live owner");
    ASSERT_EQ(destroyed, 0U);
    ASSERT_EQ(kObject->use_count(), 1UL);
    kObject->release_ref();
    ASSERT_EQ(destroyed, 1U);
}
}  // namespace

TEST("ref count: narrow storage reaches its limit and reports only the final release") {
    cinux::base::RefCount<unsigned char> references;
    for (unsigned int owner = 1; owner < 255; ++owner) {
        references.retain();
    }
    ASSERT_EQ(references.use_count(), 255U);
    for (unsigned int owner = 255; owner > 1; --owner) {
        ASSERT_FALSE(references.release());
    }
    ASSERT_TRUE(references.release());
    ASSERT_EQ(references.use_count(), 0U);
}

int main() {
    return cinux::test::RunAll();
}

TEST("ref count: concurrent owners leave exactly the original reference") {
    cinux::base::RefCount<> references;
    const auto              kWorker = [&references] {
        for (unsigned int turn = 0; turn < 100000; ++turn) {
            references.retain();
            const bool kLast = references.release();
            cinux::base::safety::Check(!kLast, "concurrent owner consumed the original reference");
        }
    };
    {
        const std::jthread kFirst(kWorker);
        const std::jthread kSecond(kWorker);
    }
    ASSERT_EQ(references.use_count(), 1UL);
    ASSERT_TRUE(references.release());
}

TEST("spin mutex: concurrent critical sections retain every increment") {
    cinux::base::SpinMutex lock;
    unsigned int           count   = 0;
    const auto             kWorker = [&] {
        for (unsigned int turn = 0; turn < 100000; ++turn) {
            const auto kGuard = lock.guard();
            ++count;
        }
    };
    {
        const std::jthread kFirst(kWorker);
        const std::jthread kSecond(kWorker);
    }
    ASSERT_EQ(count, 200000U);
}

TEST("ref count: conditional acquisition never resurrects the last released owner") {
    cinux::base::RefCount<> references;
    ASSERT_TRUE(references.try_retain());
    ASSERT_FALSE(references.release());
    ASSERT_TRUE(references.release());
    ASSERT_FALSE(references.try_retain());
    ASSERT_EQ(references.use_count(), 0UL);
}

TEST("ref count: final release racing acquisition leaves no resurrected owner") {
    for (unsigned long turn = 0; turn < 100; ++turn) {
        cinux::base::RefCount<>    references;
        std::atomic<bool>          start{false};
        std::atomic<unsigned long> final_releases{0};
        std::jthread               reader([&] {
            while (!start.load()) {
                std::this_thread::yield();
            }
            if (references.try_retain() && references.release()) {
                final_releases.fetch_add(1);
            }
        });
        start.store(true);
        if (references.release()) {
            final_releases.fetch_add(1);
        }
        reader.join();
        ASSERT_EQ(final_releases.load(), 1UL);
        ASSERT_FALSE(references.try_retain());
    }
}
