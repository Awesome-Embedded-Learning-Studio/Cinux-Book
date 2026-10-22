#include "../framework/framework.hpp"
#include "cinux/async.hpp"
#include "test_assert.hpp"

namespace {

struct FakeOwner {
    unsigned long fires  = 0;
    unsigned long status = 0;
    unsigned long length = 0;
};

void count_fire(FakeOwner& owner, unsigned long status, unsigned long length) {
    ++owner.fires;
    owner.status = status;
    owner.length = length;
}

struct RearmingOwner {
    cinux::base::AsyncCallback<RearmingOwner> callback;
    unsigned long                             fires             = 0;
    bool                                      disarmed_on_entry = true;
};

void rearm(RearmingOwner& owner, unsigned long /*status*/, unsigned long /*length*/) {
    owner.disarmed_on_entry = owner.disarmed_on_entry && !owner.callback.armed();
    ++owner.fires;
    owner.callback.arm(&rearm, &owner);
}

}  // namespace

TEST("async: an armed callback fires once on its owner") {
    FakeOwner owner;

    cinux::base::AsyncCallback<FakeOwner> callback;
    ASSERT_FALSE(callback.armed());
    callback.invoke(1UL, 2UL);
    ASSERT_EQ(owner.fires, 0UL);

    callback.arm(&count_fire, &owner);
    ASSERT_TRUE(callback.armed());
    callback.invoke(7UL, 34UL);
    ASSERT_EQ(owner.fires, 1UL);
    ASSERT_EQ(owner.status, 7UL);
    ASSERT_EQ(owner.length, 34UL);

    callback.invoke(9UL, 10UL);
    ASSERT_EQ(owner.fires, 1UL);
    ASSERT_FALSE(callback.armed());
}

TEST("async: disarm clears the seat without firing") {
    FakeOwner owner;

    cinux::base::AsyncCallback<FakeOwner> callback;
    callback.arm(&count_fire, &owner);
    callback.disarm();
    ASSERT_FALSE(callback.armed());
    callback.invoke(1UL, 1UL);
    ASSERT_EQ(owner.fires, 0UL);
}

TEST("async: re-arming replaces the seat for the next shot") {
    FakeOwner owner;

    cinux::base::AsyncCallback<FakeOwner> callback;
    callback.arm(&count_fire, &owner);
    callback.invoke(1UL, 1UL);
    callback.arm(&count_fire, &owner);
    callback.invoke(5UL, 6UL);
    ASSERT_EQ(owner.fires, 2UL);
    ASSERT_EQ(owner.status, 5UL);
}

TEST("async: a handler can arm the next completion before returning") {
    RearmingOwner owner;
    owner.callback.arm(&rearm, &owner);
    for (unsigned long round = 0; round < 32; ++round) {
        owner.callback.invoke(1UL, 8UL);
        ASSERT_TRUE(owner.callback.armed());
        ASSERT_EQ(owner.fires, round + 1);
    }
    ASSERT_TRUE(owner.disarmed_on_entry);
    owner.callback.disarm();
    owner.callback.invoke(1UL, 8UL);
    ASSERT_EQ(owner.fires, 32UL);
}

int main() {
    return cinux::test::RunAll();
}
