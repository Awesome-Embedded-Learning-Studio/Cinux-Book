#include <stdint.h>

#include <array>
#include <atomic>

#include "cinux/assert.hpp"
#include "cinux/bit_ops/bitmask.hpp"
#include "cinux/memory.hpp"
#include "framework_kernel.hpp"
#include "kernel/arch/x86_64/instructions.hpp"
#include "kernel/arch/x86_64/irq_guard.hpp"
#include "kernel/arch/x86_64/per_cpu.hpp"
#include "kernel/arch/x86_64/smp.hpp"
#include "kernel/driver/ahci/ahci.hpp"
#include "kernel/driver/block/block_cache.hpp"
#include "kernel/driver/block/block_device_concept.hpp"
#include "kernel/driver/block/block_queue.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/task.hpp"
#include "test_assert.hpp"
#include "test_case.hpp"  // NOLINT(misc-include-cleaner) consumed by the TEST() macro body

namespace {

constexpr unsigned long kSector  = 512;
constexpr unsigned      kRounds  = 8;
constexpr uint64_t      kBaseLba = 8040;

static_assert(cinux::driver::BlockDevice<cinux::driver::Ahci>,
              "the ahci drive satisfies the block face");

cinux::driver::BlockQueue*        g_queue = nullptr;
std::atomic<unsigned long>        g_failures{0};
std::atomic<unsigned long>        g_completed{0};
std::atomic<unsigned int>         g_cpu_mask{0};
std::array<unsigned long long, 3> g_workers{};
unsigned int                      g_worker_count = 0;

void meet_on_both_cpus() {
    const cinux::arch::IrqGuard kGuard;
    g_cpu_mask.fetch_or(cinux::base::bit::MaskBit<unsigned int>(
                            static_cast<unsigned char>(cinux::arch::per_cpu::PerCpu::self().cpu_id))
                            .raw,
                        std::memory_order_acq_rel);
    while (g_cpu_mask.load(std::memory_order_acquire) != 3) {
        cinux::arch::CpuRelax();
    }
}

void worker_task() {
    meet_on_both_cpus();
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
            ++g_completed;
            return;
        }
    }
    ++g_completed;
}

void seat_built(cinux::proc::Task* task) {
    cinux::base::safety::Check(task != nullptr, "block worker failed to build");
    auto& scheduler = cinux::proc::Scheduler::self();
    scheduler.seat(*task, false);
    g_workers[g_worker_count++] = task->pid;
    scheduler.unblock(*task);
}

void await_workers() {
    ASSERT_TRUE(cinux::arch::smp::BootSecondaryCpus());
    auto& scheduler = cinux::proc::Scheduler::self();
    scheduler.set_preemption(true);
    cinux::arch::smp::EnableSecondaryDispatch();
    const cinux::arch::IrqGuard kGuard;
    for (;;) {
        bool live = false;
        for (const auto kPid : g_workers) {
            live =
                scheduler.with_task(kPid, []([[maybe_unused]] cinux::proc::Task& task) {}) || live;
        }
        if (!live) {
            break;
        }
        scheduler.run_until_done();
        cinux::arch::EnableIrqAndHalt();
    }
    ASSERT_EQ(g_completed.load(), 3UL);
    ASSERT_EQ(g_cpu_mask.load(), 3U);
}

constexpr unsigned long    kCacheBase  = 7808;
constexpr unsigned long    kCachePages = 12;
constexpr unsigned long    kPageBytes  = 4096;
cinux::driver::BlockCache* g_cache     = nullptr;
std::atomic<unsigned long> g_reader_id{0};

uint8_t page_tag(unsigned long page, unsigned long byte) {
    return static_cast<uint8_t>((page * 31) + (byte / kSector));
}

void cache_reader_task() {
    meet_on_both_cpus();
    const unsigned long kReader = g_reader_id++;
    for (unsigned long round = 0; round < 16; ++round) {
        const unsigned long       kPage = ((round * 3) + kReader) % kCachePages;
        std::array<uint8_t, 1024> bytes{};
        if (!g_cache->read(cinux::driver::Lba{.value = kCacheBase + (kPage * 8) + 2}, bytes.data(),
                           bytes.size())) {
            ++g_failures;
            ++g_completed;
            return;
        }
        // Other readers evict this slot before the caller consumes its
        // copy. A cache read must leave caller-owned bytes stable.
        cinux::proc::Scheduler::self().yield();
        for (unsigned long index = 0; index < bytes.size(); ++index) {
            if (bytes[index] != page_tag(kPage, index + (2 * kSector))) {
                ++g_failures;
                ++g_completed;
                return;
            }
        }
    }
    ++g_completed;
}

}  // namespace

TEST("block: the queued drive answers single read and write") {
    cinux::driver::BlockQueue    queue(cinux::driver::Ahci::self());
    std::array<uint8_t, kSector> pattern{};
    std::array<uint8_t, kSector> readback{};
    for (unsigned long index = 0; index < pattern.size(); ++index) {
        pattern[index] = static_cast<uint8_t>(0xC0 + index);
    }
    ASSERT_TRUE(queue.write(cinux::driver::Lba{.value = 8000}, cinux::driver::BlockSpan{.value = 1},
                            pattern.data()));
    ASSERT_TRUE(queue.read(cinux::driver::Lba{.value = 8000}, cinux::driver::BlockSpan{.value = 1},
                           readback.data()));
    ASSERT_TRUE(cinux::base::EqualBytes(pattern.data(), readback.data(), pattern.size()));
    ASSERT_TRUE(queue.flush());
}

TEST("block: many tasks hammer the queue and each round trip holds") {
    cinux::driver::BlockQueue queue(cinux::driver::Ahci::self());
    g_queue        = &queue;
    g_failures     = 0;
    g_completed    = 0;
    g_cpu_mask     = 0;
    g_worker_count = 0;
    seat_built(cinux::proc::TaskBuilder{}.set_entry(worker_task).set_name("b0").build());
    seat_built(cinux::proc::TaskBuilder{}.set_entry(worker_task).set_name("b1").build());
    seat_built(cinux::proc::TaskBuilder{}.set_entry(worker_task).set_name("b2").build());
    await_workers();
    g_queue = nullptr;
    ASSERT_EQ(g_failures.load(), 0UL);
}

TEST("block: concurrent cache readers retain copies across sleeping fills and eviction") {
    cinux::driver::BlockQueue       queue(cinux::driver::Ahci::self());
    std::array<uint8_t, kPageBytes> bytes{};
    for (unsigned long page = 0; page < kCachePages; ++page) {
        for (unsigned long index = 0; index < bytes.size(); ++index) {
            bytes[index] = page_tag(page, index);
        }
        ASSERT_TRUE(queue.write(cinux::driver::Lba{.value = kCacheBase + (page * 8)},
                                cinux::driver::BlockSpan{.value = 8}, bytes.data()));
    }
    cinux::driver::BlockCache cache(queue, 1);
    g_cache        = &cache;
    g_reader_id    = 0;
    g_failures     = 0;
    g_completed    = 0;
    g_cpu_mask     = 0;
    g_worker_count = 0;
    for (unsigned int index = 0; index < 3; ++index) {
        seat_built(
            cinux::proc::TaskBuilder{}.set_entry(cache_reader_task).set_name("cache").build());
    }
    await_workers();
    g_cache = nullptr;
    ASSERT_EQ(g_failures.load(), 0UL);
    ASSERT_EQ(g_reader_id.load(), 3UL);
}
