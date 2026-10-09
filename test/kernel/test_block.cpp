#include <stdint.h>

#include <array>

#include "cinux/assert.hpp"
#include "cinux/memory.hpp"
#include "framework_kernel.hpp"
#include "kernel/driver/ahci/ahci.hpp"
#include "kernel/driver/block/block_device_concept.hpp"
#include "kernel/driver/block/block_queue.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/task.hpp"
#include "test_assert.hpp"
#include "test_case.hpp"  // NOLINT(misc-include-cleaner) consumed by the TEST() macro body

namespace {

constexpr unsigned long kSector  = 512;
constexpr unsigned      kRounds  = 8;
constexpr uint64_t      kBaseLba = 64;

static_assert(cinux::driver::BlockDevice<cinux::driver::Ahci>,
              "the ahci drive satisfies the block face");

cinux::driver::BlockQueue* g_queue    = nullptr;
unsigned long              g_failures = 0;

void worker_task() {
    for (unsigned round = 0; round < kRounds; ++round) {
        std::array<uint8_t, kSector> pattern{};
        std::array<uint8_t, kSector> readback{};
        const uint64_t               kLba = kBaseLba + round;
        for (unsigned long index = 0; index < pattern.size(); ++index) {
            pattern[index] = static_cast<uint8_t>(index + round);
        }
        const cinux::driver::Lba       kFirst{.value = kLba};
        const cinux::driver::BlockSpan kOne{.value = 1};
        if (!g_queue->write(kFirst, kOne, pattern.data()) ||
            !g_queue->read(kFirst, kOne, readback.data()) ||
            !cinux::base::EqualBytes(pattern.data(), readback.data(), pattern.size()) ||
            !g_queue->flush()) {
            ++g_failures;
            return;
        }
    }
}

void seat_built(cinux::proc::Task* task) {
    cinux::base::safety::Check(task != nullptr, "block worker failed to build");
    cinux::proc::Scheduler::self().seat(*task);
}

}  // namespace

TEST("block: the queued drive answers single read and write") {
    cinux::driver::BlockQueue    queue(cinux::driver::Ahci::self());
    std::array<uint8_t, kSector> pattern{};
    std::array<uint8_t, kSector> readback{};
    for (unsigned long index = 0; index < pattern.size(); ++index) {
        pattern[index] = static_cast<uint8_t>(0xC0 + index);
    }
    ASSERT_TRUE(queue.write(cinux::driver::Lba{.value = 32}, cinux::driver::BlockSpan{.value = 1},
                            pattern.data()));
    ASSERT_TRUE(queue.read(cinux::driver::Lba{.value = 32}, cinux::driver::BlockSpan{.value = 1},
                           readback.data()));
    ASSERT_TRUE(cinux::base::EqualBytes(pattern.data(), readback.data(), pattern.size()));
    ASSERT_TRUE(queue.flush());
}

TEST("block: many tasks hammer the queue and each round trip holds") {
    cinux::driver::BlockQueue queue(cinux::driver::Ahci::self());
    g_queue    = &queue;
    g_failures = 0;
    seat_built(cinux::proc::TaskBuilder{}.set_entry(worker_task).set_name("b0").build());
    seat_built(cinux::proc::TaskBuilder{}.set_entry(worker_task).set_name("b1").build());
    seat_built(cinux::proc::TaskBuilder{}.set_entry(worker_task).set_name("b2").build());
    cinux::proc::Scheduler::self().run_until_done();
    g_queue = nullptr;
    ASSERT_EQ(g_failures, 0UL);
}
