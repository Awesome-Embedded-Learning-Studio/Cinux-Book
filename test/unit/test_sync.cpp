#include "../framework/framework.hpp"
#include "kernel/proc/sync.hpp"
#include "test_assert.hpp"

namespace {

TEST("sync: spinlock counts holders and names them") {
    cinux::proc::SpinLedger ledger;
    cinux::proc::Spinlock   lock(&ledger);
    ASSERT_TRUE(ledger.depth() == 0);
    {
        const cinux::proc::SpinGuard kGuard(lock);
        ASSERT_TRUE(ledger.depth() == 1);
        ASSERT_TRUE(lock.holder_name() != nullptr);
    }
    ASSERT_TRUE(ledger.depth() == 0);
}

TEST("sync: nested spinlocks unwind their count") {
    cinux::proc::SpinLedger ledger;
    cinux::proc::Spinlock   outer(&ledger);
    cinux::proc::Spinlock   inner(&ledger);
    {
        const cinux::proc::SpinGuard kOuter(outer);
        {
            const cinux::proc::SpinGuard kInner(inner);
            ASSERT_TRUE(ledger.depth() == 2);
        }
        ASSERT_TRUE(ledger.depth() == 1);
    }
    ASSERT_TRUE(ledger.depth() == 0);
}

TEST("sync: uncontended mutex lock and unlock round-trips") {
    const cinux::proc::SpinLedger kLedger;
    cinux::proc::Mutex            mutex;
    {
        const cinux::proc::MutexGuard kGuard(mutex);
    }
    ASSERT_TRUE(kLedger.depth() == 0);
}

TEST("sync: semaphore counts units and never parks while stocked") {
    cinux::proc::Semaphore slots(2);
    slots.wait();
    slots.wait();
    slots.post();
    slots.wait();
    slots.post();
    slots.post();
}

TEST("sync: a completion finishes once and publishes its verdict") {
    cinux::proc::Completion done;
    ASSERT_FALSE(done.finished());
    ASSERT_TRUE(done.finish(true));
    ASSERT_FALSE(done.finish(false));
    ASSERT_TRUE(done.finished());
    ASSERT_TRUE(done.success());
}

TEST("sync: a stocked completion answers wait without parking") {
    cinux::proc::Completion done;
    done.finish(false);
    ASSERT_TRUE(done.finished());
    ASSERT_FALSE(done.wait());
}

TEST("sync: rearm returns a terminal completion to pending") {
    cinux::proc::Completion done;
    done.finish(true);
    done.rearm();
    ASSERT_FALSE(done.finished());
    ASSERT_TRUE(done.finish(false));
    ASSERT_FALSE(done.success());
}

}  // namespace

int main() {
    return cinux::test::RunAll();
}
