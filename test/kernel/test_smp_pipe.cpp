#include <array>
#include <atomic>

#include "cinux/bit_ops/bitmask.hpp"
#include "cinux/own.hpp"
#include "kernel/arch/x86_64/instructions.hpp"
#include "kernel/arch/x86_64/irq_guard.hpp"
#include "kernel/arch/x86_64/per_cpu.hpp"
#include "kernel/arch/x86_64/smp.hpp"
#include "kernel/fs/pipe.hpp"
#include "kernel/proc/lockdep.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/task.hpp"
#include "test_assert.hpp"
#include "test_case.hpp"

namespace {

cinux::fs::Pipe*          g_pipe = nullptr;
std::atomic<unsigned int> g_cpus{0};
std::atomic<unsigned int> g_eof{0};

void eof_reader() {
    {
        const cinux::arch::IrqGuard kIrq;
        const auto                  kCpu = cinux::arch::per_cpu::PerCpu::self().cpu_id;
        g_cpus.fetch_or(
            cinux::base::bit::MaskBit<unsigned int>(static_cast<unsigned char>(kCpu)).raw,
            std::memory_order_acq_rel);
        while (g_cpus.load(std::memory_order_acquire) != 3) {
            cinux::arch::CpuRelax();
        }
    }
    unsigned char byte  = 0;
    const auto    kRead = g_pipe->pull(&byte, 1);
    if (kRead.ok() && kRead.value() == 0) {
        g_eof.fetch_add(1, std::memory_order_release);
    }
}

bool blocked(unsigned long long pid) {
    bool waiting = false;
    cinux::proc::Scheduler::self().with_task(pid, [&waiting](cinux::proc::Task& task) {
        waiting = task.state == cinux::proc::TaskState::kBlocked;
    });
    return waiting;
}

bool registered(unsigned long long pid) {
    return cinux::proc::Scheduler::self().with_task(
        pid, []([[maybe_unused]] cinux::proc::Task& task) {});
}

void all_parked_readers_receive_eof() {
    ASSERT_TRUE(cinux::arch::smp::BootSecondaryCpus());
    cinux::fs::Pipe pipe;
    pipe.add_writer();
    g_pipe                                      = &pipe;
    auto&                             scheduler = cinux::proc::Scheduler::self();
    std::array<unsigned long long, 2> readers{};
    for (auto& pid : readers) {
        auto* reader = cinux::proc::TaskBuilder{}.set_entry(eof_reader).set_name("eof").build();
        ASSERT_TRUE(reader != nullptr);
        scheduler.seat(*reader, false);
        pid = reader->pid;
        scheduler.unblock(*reader);
    }
    scheduler.set_preemption(true);
    cinux::arch::smp::EnableSecondaryDispatch();
    while (!blocked(readers[0]) || !blocked(readers[1])) {
        scheduler.run_until_done();
    }
    pipe.drop_writer();
    while (registered(readers[0]) || registered(readers[1])) {
        scheduler.run_until_done();
    }
    ASSERT_EQ(g_cpus.load(std::memory_order_acquire), 3U);
    ASSERT_EQ(g_eof.load(std::memory_order_acquire), 2U);
    g_pipe = nullptr;
}

void destroyed_pipe_locks_leave_the_registry() {
    std::array<cinux::base::Own<cinux::fs::Pipe>, cinux::proc::LockOrder::kLocks + 1> pipes;
    for (auto& pipe : pipes) {
        pipe = cinux::base::MakeOwn<cinux::fs::Pipe>();
    }
    for (auto& pipe : pipes) {
        pipe->add_writer();
        const unsigned char kSent = 42;
        unsigned char       heard = 0;
        ASSERT_TRUE(pipe->push(&kSent, 1).ok());
        const auto kRead = pipe->pull(&heard, 1);
        ASSERT_TRUE(kRead.ok());
        ASSERT_EQ(kRead.value(), 1UL);
        ASSERT_EQ(heard, kSent);
        pipe->drop_writer();
        pipe.reset();
    }
}

[[gnu::used, gnu::section(".test_cases")]] constinit const cinux::test::TestCase kEofReaders{
    .name = "smp pipe: both parked readers observe EOF after the last writer closes",
    .body = all_parked_readers_receive_eof,
};

[[gnu::used, gnu::section(".test_cases")]] constinit const cinux::test::TestCase kPipeLifetime{
    .name = "smp pipe: destroyed locks do not exhaust the live identity registry",
    .body = destroyed_pipe_locks_leave_the_registry,
};

}
