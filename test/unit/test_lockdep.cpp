#include "framework.hpp"
#include "kernel/proc/lockdep.hpp"
#include "test_assert.hpp"

namespace {
using Order  = cinux::proc::LockOrder;
using Status = Order::Status;

void record_pair(Order& order, const void* earlier, const void* later) {
    ASSERT_TRUE(order.acquire(earlier) == Status::kOk);
    ASSERT_TRUE(order.acquire(later) == Status::kOk);
    ASSERT_TRUE(order.release(later) == Status::kOk);
    ASSERT_TRUE(order.release(earlier) == Status::kOk);
}

TEST("lockdep: remembers AB then rejects BA") {
    Order order;
    int   alpha = 0;
    int   beta  = 0;
    ASSERT_TRUE(order.acquire(&alpha) == Status::kOk);
    ASSERT_TRUE(order.acquire(&beta) == Status::kOk);
    ASSERT_TRUE(order.release(&beta) == Status::kOk);
    ASSERT_TRUE(order.release(&alpha) == Status::kOk);
    ASSERT_TRUE(order.acquire(&beta) == Status::kOk);
    ASSERT_TRUE(order.acquire(&alpha) == Status::kCycle);
    ASSERT_TRUE(order.release(&beta) == Status::kOk);
}

TEST("lockdep: rejects transitive cycle") {
    Order order;
    int   locks[3]{};
    for (unsigned int pair = 0; pair < 2; ++pair) {
        record_pair(order, &locks[pair], &locks[pair + 1]);
    }
    ASSERT_TRUE(order.acquire(&locks[2]) == Status::kOk);
    ASSERT_TRUE(order.acquire(&locks[0]) == Status::kCycle);
    ASSERT_TRUE(order.release(&locks[2]) == Status::kOk);
}

TEST("lockdep: invalid operations leave held stack intact") {
    Order order;
    int   locks[3]{};
    ASSERT_TRUE(order.acquire(&locks[0]) == Status::kOk);
    ASSERT_TRUE(order.acquire(&locks[0]) == Status::kRecursive);
    ASSERT_TRUE(order.acquire(&locks[1]) == Status::kOk);
    ASSERT_TRUE(order.release(&locks[0]) == Status::kOutOfOrder);
    ASSERT_TRUE(order.release(&locks[2]) == Status::kNotHeld);
    ASSERT_FALSE(order.forget(&locks[0]));
    ASSERT_TRUE(order.release(&locks[1]) == Status::kOk);
    ASSERT_TRUE(order.release(&locks[0]) == Status::kOk);
}

TEST("lockdep: forgetting a destroyed lock permits address reuse") {
    Order order;
    int   locks[2]{};
    ASSERT_TRUE(order.acquire(&locks[0]) == Status::kOk);
    ASSERT_TRUE(order.acquire(&locks[1]) == Status::kOk);
    ASSERT_TRUE(order.release(&locks[1]) == Status::kOk);
    ASSERT_TRUE(order.release(&locks[0]) == Status::kOk);
    ASSERT_TRUE(order.forget(&locks[0]));
    ASSERT_TRUE(order.acquire(&locks[1]) == Status::kOk);
    ASSERT_TRUE(order.acquire(&locks[0]) == Status::kOk);
}

TEST("lockdep: held depth exhaustion fails visibly") {
    Order order;
    int   locks[Order::kDepth + 1]{};
    for (unsigned int slot = 0; slot < Order::kDepth; ++slot) {
        ASSERT_TRUE(order.acquire(&locks[slot]) == Status::kOk);
    }
    ASSERT_TRUE(order.acquire(&locks[Order::kDepth]) == Status::kCapacity);
    for (unsigned int slot = Order::kDepth; slot != 0; --slot) {
        ASSERT_TRUE(order.release(&locks[slot - 1]) == Status::kOk);
    }
}

TEST("lockdep: graph capacity is reclaimed on destruction") {
    Order order;
    int   locks[Order::kLocks + 1]{};
    for (unsigned int slot = 0; slot < Order::kLocks; ++slot) {
        ASSERT_TRUE(order.acquire(&locks[slot]) == Status::kOk);
        ASSERT_TRUE(order.release(&locks[slot]) == Status::kOk);
    }
    ASSERT_TRUE(order.acquire(&locks[Order::kLocks]) == Status::kCapacity);
    ASSERT_TRUE(order.forget(&locks[0]));
    ASSERT_TRUE(order.acquire(&locks[Order::kLocks]) == Status::kOk);
}
}  // namespace

int main() {
    return cinux::test::RunAll();
}
