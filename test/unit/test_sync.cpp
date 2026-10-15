#include "../framework/framework.hpp"
#include "kernel/proc/checks_config.hpp"
#include "kernel/proc/lockdep.hpp"
#include "kernel/proc/sync.hpp"
#include "test_assert.hpp"

namespace {

constexpr unsigned int kMaxDumpLines = 8;

struct DumpLog {
    unsigned int       count                    = 0;
    const char*        labels[kMaxDumpLines]    = {};
    unsigned long long addresses[kMaxDumpLines] = {};
};

DumpLog            g_dump_log;
unsigned long long g_lock_address = 0;

void capture_dump_line(const char* label, unsigned long long address) {
    if (g_dump_log.count < kMaxDumpLines) {
        g_dump_log.labels[g_dump_log.count]    = label;
        g_dump_log.addresses[g_dump_log.count] = address;
    }
    ++g_dump_log.count;
}

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

TEST("sync: a named lock answers the dump with its label") {
    if constexpr (!cinux::proc::kLockdepEnabled) {
        return;
    }
    cinux::proc::SpinLedger ledger;
    cinux::proc::Spinlock   lock(&ledger);
    lock.set_name("ramfs volume");
    g_dump_log     = DumpLog{};
    g_lock_address = reinterpret_cast<unsigned long long>(&lock);
    cinux::proc::InstallLockdepSink(&capture_dump_line);
    {
        const cinux::proc::SpinGuard kGuard(lock);
        ledger.dump_held();
    }
    cinux::proc::InstallLockdepSink(nullptr);
    ASSERT_TRUE(g_dump_log.count == 1);
    ASSERT_TRUE(g_dump_log.labels[0] != nullptr);
    ASSERT_STREQ(g_dump_log.labels[0], "ramfs volume");
    ASSERT_TRUE(g_dump_log.addresses[0] == g_lock_address);
}

TEST("sync: an unnamed lock dumps by address and retire clears the name") {
    if constexpr (!cinux::proc::kLockdepEnabled) {
        return;
    }
    cinux::proc::SpinLedger ledger;
    {
        cinux::proc::Spinlock lock(&ledger);
        g_dump_log     = DumpLog{};
        g_lock_address = reinterpret_cast<unsigned long long>(&lock);
        cinux::proc::InstallLockdepSink(&capture_dump_line);
        {
            const cinux::proc::SpinGuard kGuard(lock);
            ledger.dump_held();
        }
        cinux::proc::InstallLockdepSink(nullptr);
        ASSERT_TRUE(g_dump_log.count == 1);
        ASSERT_TRUE(g_dump_log.labels[0] == nullptr);
        ASSERT_TRUE(g_dump_log.addresses[0] == g_lock_address);
        ledger.forget(&lock);
    }
    cinux::proc::Spinlock fresh(&ledger);
    fresh.set_name("reuse");
    g_dump_log = DumpLog{};
    cinux::proc::InstallLockdepSink(&capture_dump_line);
    {
        const cinux::proc::SpinGuard kGuard(fresh);
        ledger.dump_held();
    }
    cinux::proc::InstallLockdepSink(nullptr);
    ASSERT_TRUE(g_dump_log.count == 1);
    ASSERT_TRUE(g_dump_log.labels[0] != nullptr);
    ASSERT_STREQ(g_dump_log.labels[0], "reuse");
}

}  // namespace

int main() {
    return cinux::test::RunAll();
}
