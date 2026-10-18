#include "cinux/assert.hpp"
#include "cinux/result.hpp"
#include "framework_kernel.hpp"
#include "kernel/fs/pipe.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/task.hpp"
#include "test_assert.hpp"
#include "test_case.hpp"  // NOLINT(misc-include-cleaner) consumed by the TEST() macro body

namespace {

/// The small-run round trip: eight bytes in, the same eight out.
void check_round_trip(cinux::fs::Pipe& pipe) {
    const unsigned char                      kSent[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    const cinux::base::Result<unsigned long> kPut     = pipe.push(kSent, sizeof(kSent));
    ASSERT_TRUE(kPut.ok());
    ASSERT_EQ(kPut.value(), 8UL);
    unsigned char                            heard[8] = {};
    const cinux::base::Result<unsigned long> kTaken   = pipe.pull(heard, sizeof(heard));
    ASSERT_TRUE(kTaken.ok());
    ASSERT_EQ(kTaken.value(), 8UL);
    for (unsigned long index = 0; index < sizeof(kSent); ++index) {
        ASSERT_EQ(heard[index], kSent[index]);
    }
}

/// The capacity boundary: the ring holds its full budget and hands it
/// all back, then the last writer's farewell reads as zero.
void check_capacity_and_farewell(cinux::fs::Pipe& pipe) {
    unsigned char                            filler[cinux::fs::kPipeBytes] = {};
    const cinux::base::Result<unsigned long> kFilled = pipe.push(filler, sizeof(filler));
    ASSERT_TRUE(kFilled.ok());
    ASSERT_EQ(kFilled.value(), cinux::fs::kPipeBytes);
    unsigned char                            drained[cinux::fs::kPipeBytes] = {};
    const cinux::base::Result<unsigned long> kEmptied = pipe.pull(drained, sizeof(drained));
    ASSERT_TRUE(kEmptied.ok());
    ASSERT_EQ(kEmptied.value(), cinux::fs::kPipeBytes);
    pipe.drop_writer();
    const cinux::base::Result<unsigned long> kFarewell = pipe.pull(drained, 1);
    ASSERT_TRUE(kFarewell.ok());
    ASSERT_EQ(kFarewell.value(), 0UL);
}

TEST("pipe: bytes cross, the ring fills exactly, farewell reads zero") {
    cinux::fs::Pipe pipe;
    pipe.add_writer();
    check_round_trip(pipe);
    check_capacity_and_farewell(pipe);
}

/// The readiness vocabulary ppoll rides: bytes waiting, room to write,
/// and the writer census turning to farewell. Split in two so each
/// helper stays under the branch budget the test world enforces.
void check_faces_halfway(cinux::fs::Pipe& pipe) {
    ASSERT_TRUE(!pipe.has_bytes());
    ASSERT_TRUE(!pipe.writers_gone());
    ASSERT_TRUE(pipe.has_room());
    const unsigned char kOne = 7;
    ASSERT_TRUE(pipe.push(&kOne, 1).ok());
    ASSERT_TRUE(pipe.has_bytes());
    ASSERT_TRUE(pipe.has_room());
}

void check_faces_full(cinux::fs::Pipe& pipe) {
    unsigned char filler[cinux::fs::kPipeBytes - 1] = {};
    ASSERT_TRUE(pipe.push(filler, sizeof(filler)).ok());
    ASSERT_TRUE(!pipe.has_room());
    pipe.drop_writer();
    ASSERT_TRUE(pipe.writers_gone());
    ASSERT_TRUE(pipe.has_bytes());
}

TEST("pipe: the poll faces read bytes, room, and the farewell") {
    cinux::fs::Pipe pipe;
    pipe.add_writer();
    check_faces_halfway(pipe);
    check_faces_full(pipe);
}

cinux::fs::Pipe* g_crossing_pipe = nullptr;
bool             g_reader_parked = false;
unsigned char    g_heard[4]      = {};

void reader_task() {
    const cinux::base::Result<unsigned long> kTaken =
        g_crossing_pipe->pull(g_heard, sizeof(g_heard));
    g_reader_parked = kTaken.ok() && kTaken.value() == sizeof(g_heard);
}

TEST("pipe: a parked reader wakes when the writer arrives") {
    cinux::fs::Pipe pipe;
    pipe.add_writer();
    g_crossing_pipe       = &pipe;
    g_reader_parked       = false;
    auto&       scheduler = cinux::proc::Scheduler::self();
    auto* const kReader =
        cinux::proc::TaskBuilder{}.set_entry(reader_task).set_name("pipe_reader").build();
    cinux::base::safety::Check(kReader != nullptr, "pipe reader task failed to build");
    scheduler.seat(*kReader);
    scheduler.run_until_done();
    ASSERT_TRUE(kReader->state == cinux::proc::TaskState::kBlocked);
    const unsigned char kSent[4] = {9, 8, 7, 6};
    ASSERT_TRUE(pipe.push(kSent, sizeof(kSent)).ok());
    scheduler.run_until_done();
    ASSERT_TRUE(g_reader_parked);
    for (unsigned long index = 0; index < sizeof(kSent); ++index) {
        ASSERT_EQ(g_heard[index], kSent[index]);
    }
    g_crossing_pipe = nullptr;
}

}  // namespace
