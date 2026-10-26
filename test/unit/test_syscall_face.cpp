#include <poll.h>
#include <time.h>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <sys/stat.h>
#include <sys/syscall.h>

#include "../framework/framework.hpp"
#include "cinux/memory.hpp"
#include "cinux/result.hpp"
#include "cinux/shared.hpp"
#include "kernel/fs/file_table.hpp"
#include "kernel/fs/vfs.hpp"
#include "kernel/mm/vma.hpp"
#include "kernel/proc/proc_config.hpp"
#include "kernel/proc/sched_class.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/syscall/sys_close.hpp"
#include "kernel/syscall/sys_fcntl.hpp"
#include "kernel/syscall/sys_getrandom.hpp"
#include "kernel/syscall/sys_mmap.hpp"
#include "kernel/syscall/sys_open.hpp"
#include "kernel/syscall/sys_openat.hpp"
#include "kernel/syscall/sys_pipe.hpp"
#include "kernel/syscall/sys_pread.hpp"
#include "kernel/syscall/sys_prlimit.hpp"
#include "kernel/syscall/sys_sendfile.hpp"
#include "kernel/syscall/sys_sigtimedwait.hpp"
#include "kernel/syscall/sys_stat.hpp"
#include "kernel/syscall/sys_tgkill.hpp"
#include "kernel/syscall/sys_uname.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"
#include "test_assert.hpp"
#include "test_case.hpp"  // NOLINT(misc-include-cleaner) consumed by the TEST() macro body

// Isolate the *at argument adapters from the VFS implementation.
namespace cinux::syscall {

unsigned long FileDescriptorLimit() {
    return CurrentTask()->limits[7][0];
}

long long HandleOpen(const SyscallFrame& /*frame*/) {
    return 101;
}
long long HandleStat(const SyscallFrame& /*frame*/) {
    return 102;
}
int ErrnoFrom(base::KernelError error) {
    switch (error) {
    case base::KernelError::kWouldBlock:
        return 11;
    case base::KernelError::kInterrupted:
        return 4;
    case base::KernelError::kBrokenPipe:
        return 32;
    default:
        return 5;
    }
}
base::Result<void> ReclaimUserSpan(const proc::Task& self, unsigned long start,
                                   unsigned long bytes) {
    return self.vmas->unregister_range(start, start + bytes);
}

}  // namespace cinux::syscall

namespace {

// Compare against an independent consumer's ABI, including fields the
// teaching FsStat does not yet supply. A compact FsStat is not a wire record.
using WireStat = cinux::syscall::SyscallStat;
static_assert(static_cast<int>(cinux::syscall::SyscallNr::kTgkill) == SYS_tgkill);
static_assert(static_cast<int>(cinux::syscall::SyscallNr::kTkill) == SYS_tkill);
static_assert(sizeof(WireStat) == sizeof(struct stat));
static_assert(static_cast<int>(cinux::syscall::SyscallNr::kMmap) == SYS_mmap);
static_assert(static_cast<int>(cinux::syscall::SyscallNr::kMprotect) == SYS_mprotect);
static_assert(static_cast<int>(cinux::syscall::SyscallNr::kMunmap) == SYS_munmap);
static_assert(static_cast<int>(cinux::syscall::SyscallNr::kBrk) == SYS_brk);
static_assert(static_cast<int>(cinux::syscall::SyscallNr::kPread64) == SYS_pread64);
static_assert(static_cast<int>(cinux::syscall::SyscallNr::kRtSigtimedwait) == SYS_rt_sigtimedwait);
static_assert(static_cast<int>(cinux::syscall::SyscallNr::kMount) == SYS_mount);
static_assert(static_cast<int>(cinux::syscall::SyscallNr::kUmount2) == SYS_umount2);
static_assert(offsetof(WireStat, dev) == offsetof(struct stat, st_dev));
static_assert(offsetof(WireStat, ino) == offsetof(struct stat, st_ino));
static_assert(offsetof(WireStat, links) == offsetof(struct stat, st_nlink));
static_assert(offsetof(WireStat, mode) == offsetof(struct stat, st_mode));
static_assert(sizeof(WireStat::mode) == sizeof(decltype(stat::st_mode)));
static_assert(offsetof(WireStat, uid) == offsetof(struct stat, st_uid));
static_assert(offsetof(WireStat, gid) == offsetof(struct stat, st_gid));
static_assert(offsetof(WireStat, rdev) == offsetof(struct stat, st_rdev));
static_assert(offsetof(WireStat, size) == offsetof(struct stat, st_size));
static_assert(offsetof(WireStat, blksize) == offsetof(struct stat, st_blksize));
static_assert(offsetof(WireStat, blocks) == offsetof(struct stat, st_blocks));
static_assert(offsetof(WireStat, atime) == offsetof(struct stat, st_atim));
static_assert(offsetof(WireStat, mtime) == offsetof(struct stat, st_mtim));
static_assert(offsetof(WireStat, ctime) == offsetof(struct stat, st_ctim));
static_assert(offsetof(WireStat, mtime_nsec) ==
              offsetof(struct stat, st_mtim) + offsetof(struct timespec, tv_nsec));

// The ppoll wire shapes, pinned the same way: the record ring 3 lays
// out must match the hosted consumer's view byte for byte.
// NOLINTNEXTLINE(misc-include-cleaner) pollfd rides glibc's private bits header
static_assert(sizeof(pollfd) == 8);
// NOLINTNEXTLINE(misc-include-cleaner) pollfd rides glibc's private bits header
static_assert(offsetof(pollfd, fd) == 0);
// NOLINTNEXTLINE(misc-include-cleaner) pollfd rides glibc's private bits header
static_assert(offsetof(pollfd, events) == 4);
// NOLINTNEXTLINE(misc-include-cleaner) pollfd rides glibc's private bits header
static_assert(offsetof(pollfd, revents) == 6);
static_assert(sizeof(timespec) == 16);
static_assert(offsetof(timespec, tv_sec) == 0);
static_assert(offsetof(timespec, tv_nsec) == 8);
// NOLINTNEXTLINE(misc-include-cleaner) the POLL* words ride glibc's private bits header
static_assert(POLLIN == 0x001);
// NOLINTNEXTLINE(misc-include-cleaner) the POLL* words ride glibc's private bits header
static_assert(POLLOUT == 0x004);
// NOLINTNEXTLINE(misc-include-cleaner) the POLL* words ride glibc's private bits header
static_assert(POLLHUP == 0x010);
// NOLINTNEXTLINE(misc-include-cleaner) the POLL* words ride glibc's private bits header
static_assert(POLLNVAL == 0x020);

constexpr unsigned long kSlot = 65;

cinux::syscall::SyscallFrame plain_frame() {
    cinux::syscall::SyscallFrame frame{};
    return frame;
}

// The task-bound faces (prlimit, prctl, fcntl) ask the scheduler who is
// calling, so those tests run inside a seated task's body, the same way
// the hosted world enters any body at all. A body ends with
// exit_current(), exactly like the signal tests' bodies do.
void (*g_face_body)() = nullptr;

class FaceSink {
public:
    void switch_tasks(cinux::proc::Task& outgoing, cinux::proc::Task& incoming);
    void enter_from_main(cinux::proc::Task& incoming);
    void return_to_main(cinux::proc::Task& outgoing);
    void reclaim(cinux::proc::Task& dead);

private:
    void run_body_of(cinux::proc::Task& incoming);
};

class FaceWorld {
public:
    FaceWorld() {
        kin.name          = "kin";
        runner.name       = "face";
        runner.stack_base = 0x1000ULL * 0x40;
        runner.parent     = &kin;
        cinux::base::CopyBytes(runner.limits, cinux::proc::kDefaultResourceLimits,
                               sizeof(runner.limits));
        cinux::proc::Scheduler::self().init(sink);
        cinux::proc::Scheduler::self().install_class(cinux::proc::DefaultFifoClass());
        cinux::proc::Scheduler::self().seat(runner);
    }

    ~FaceWorld() {
        cinux::proc::Scheduler::self().seat(kin);
        cinux::proc::Scheduler::self().run_until_done();
    }

    void run(void (*body)()) {
        g_face_body = body;
        cinux::proc::Scheduler::self().run_until_done();
    }

    FaceSink          sink;
    cinux::proc::Task kin;
    cinux::proc::Task runner;
};

void FaceSink::run_body_of([[maybe_unused]] cinux::proc::Task& incoming) {
    if (g_face_body != nullptr) {
        void (*const kBody)() = g_face_body;
        g_face_body           = nullptr;
        kBody();
        return;
    }
    cinux::proc::Scheduler::self().exit_current();
}

void FaceSink::enter_from_main(cinux::proc::Task& incoming) {
    run_body_of(incoming);
}

void FaceSink::switch_tasks(cinux::proc::Task& outgoing, cinux::proc::Task& incoming) {
    static_cast<void>(outgoing);
    run_body_of(incoming);
}

void FaceSink::return_to_main(cinux::proc::Task& outgoing) {
    static_cast<void>(outgoing);
}

void FaceSink::reclaim(cinux::proc::Task& dead) {
    static_cast<void>(dead);
}

TEST("syscall face: at-fdcwd accepts both extensions of a signed int") {
    cinux::syscall::SyscallFrame frame = plain_frame();
    for (const unsigned long long kDescriptor : {0x00000000FFFFFF9CULL, 0xFFFFFFFFFFFFFF9CULL}) {
        frame.rdi = kDescriptor;
        ASSERT_EQ(cinux::syscall::HandleOpenat(frame), 101);
        ASSERT_EQ(cinux::syscall::HandleNewfstatat(frame), 102);
    }
    frame.rdi = 7;
    ASSERT_EQ(cinux::syscall::HandleOpenat(frame), -cinux::syscall::kEnotdir);
    ASSERT_EQ(cinux::syscall::HandleNewfstatat(frame), -cinux::syscall::kEnotdir);
}

TEST("syscall face: uname fills the six slots in hosted order") {
    char record[6 * 65];
    for (char& slot : record) {
        slot = static_cast<char>(0xAA);
    }
    cinux::syscall::SyscallFrame frame = plain_frame();
    frame.rdi                          = reinterpret_cast<unsigned long long>(record);
    ASSERT_EQ(cinux::syscall::HandleUname(frame), 0);
    ASSERT_EQ(record[0], 'C');
    ASSERT_EQ(record[6], '\0');
    ASSERT_EQ(record[kSlot], 'c');
    ASSERT_EQ(record[2 * kSlot], '0');
    ASSERT_EQ(record[4 * kSlot], 'x');
    ASSERT_EQ(record[5 * kSlot], '\0');
}

void check_updated_limits(cinux::syscall::SyscallFrame& frame, unsigned long long (&old)[2]) {
    frame.rdx = 0;
    frame.r10 = reinterpret_cast<unsigned long long>(old);
    ASSERT_EQ(cinux::syscall::HandlePrlimit64(frame), 0);
    ASSERT_EQ(old[0], 5ULL);
    ASSERT_EQ(old[1], cinux::fs::kFileTableMax);
}

void run_prlimit_body() {
    unsigned long long           old[2] = {0, 0};
    cinux::syscall::SyscallFrame frame  = plain_frame();
    frame.rdi                           = 0;
    frame.r10                           = reinterpret_cast<unsigned long long>(old);
    ASSERT_EQ(cinux::syscall::HandlePrlimit64(frame), 0);
    frame.rdi = 7;
    ASSERT_EQ(cinux::syscall::HandlePrlimit64(frame), -1);
    frame.rdi = 0;
    frame.rsi = 99;
    ASSERT_EQ(cinux::syscall::HandlePrlimit64(frame), -22);
    frame.rsi                                   = 7;
    cinux::syscall::CurrentTask()->limits[7][0] = 3;
    cinux::syscall::CurrentTask()->limits[7][1] = 9;
    unsigned long long want[2]                  = {5, cinux::fs::kFileTableMax};
    frame.rdx                                   = reinterpret_cast<unsigned long long>(want);
    ASSERT_EQ(cinux::syscall::HandlePrlimit64(frame), 0);
    ASSERT_EQ(old[0], 3ULL);
    ASSERT_EQ(old[1], 9ULL);
    check_updated_limits(frame, old);
    cinux::proc::Scheduler::self().exit_current();
}

TEST("syscall face: prlimit reads, writes and refuses strangers") {
    FaceWorld world;
    world.run(run_prlimit_body);
}

void run_fcntl_seat_phase(cinux::syscall::SyscallFrame& frame, unsigned long long seat) {
    frame.rdi = seat;
    frame.rsi = 3;
    ASSERT_EQ(cinux::syscall::HandleFcntl(frame), 2);
    frame.rsi = 4;
    frame.rdx = 0x402;
    ASSERT_EQ(cinux::syscall::HandleFcntl(frame), 0);
    frame.rsi = 3;
    ASSERT_EQ(cinux::syscall::HandleFcntl(frame), 0x402);
    // The dup lands on the next seat and shares the one open.
    frame.rsi             = 0;
    frame.rdx             = 0;
    const long long kCopy = cinux::syscall::HandleFcntl(frame);
    ASSERT_EQ(kCopy, static_cast<long long>(seat + 1));
    frame.rsi = 99;
    ASSERT_EQ(cinux::syscall::HandleFcntl(frame), -22);
    frame.rdi = 9;
    frame.rsi = 3;
    ASSERT_EQ(cinux::syscall::HandleFcntl(frame), -9);
}

void run_fcntl_body() {
    cinux::syscall::SyscallFrame             frame = plain_frame();
    // A seated stream-shaped open: status starts read-write, the seat
    // bit arms and reads back through the table.
    const cinux::proc::Task* const           kSelf = cinux::proc::Scheduler::self().current();
    const cinux::base::Result<unsigned long> kFd   = kSelf->files->alloc(nullptr, nullptr, {});
    ASSERT_TRUE(kFd.ok());
    kSelf->files->get(kFd.value()).value()->status_flags = 2;
    frame.rdi                                            = kFd.value();
    frame.rsi                                            = 3;
    ASSERT_EQ(cinux::syscall::HandleFcntl(frame), 2);
    frame.rsi = 1;
    ASSERT_EQ(cinux::syscall::HandleFcntl(frame), 0);
    frame.rsi = 2;
    frame.rdx = 1;
    ASSERT_EQ(cinux::syscall::HandleFcntl(frame), 0);
    frame.rsi = 1;
    ASSERT_EQ(cinux::syscall::HandleFcntl(frame), 1);
    // The stream seats are gone with the old privilege: an unseated
    // number answers EBADF.
    frame.rdi = 1;
    frame.rsi = 3;
    ASSERT_EQ(cinux::syscall::HandleFcntl(frame), -9);
    run_fcntl_seat_phase(frame, kFd.value());
    cinux::proc::Scheduler::self().exit_current();
}

TEST("syscall face: fcntl re-arms status faces and seat bits") {
    FaceWorld world;
    world.run(run_fcntl_body);
}

void run_prctl_body() {
    cinux::syscall::SyscallFrame frame    = plain_frame();
    // PR_SET_NAME / PR_GET_NAME ride the uapi prctl op numbers.
    char                         name[16] = {};
    name[0]                               = 'f';
    name[1]                               = 'a';
    name[2]                               = 'c';
    name[3]                               = 'e';
    char back[16]                         = {};
    frame.rdi                             = 15;
    frame.rsi                             = reinterpret_cast<unsigned long long>(name);
    ASSERT_EQ(cinux::syscall::HandlePrctl(frame), 0);
    frame.rdi = 16;
    frame.rsi = reinterpret_cast<unsigned long long>(back);
    ASSERT_EQ(cinux::syscall::HandlePrctl(frame), 0);
    ASSERT_EQ(back[0], 'f');
    ASSERT_EQ(back[3], 'e');
    ASSERT_EQ(back[4], 0);
    cinux::proc::Scheduler::self().exit_current();
}

TEST("syscall face: prctl names the task and reads it back") {
    FaceWorld world;
    world.run(run_prctl_body);
}

TEST("syscall face: getrandom counts the bytes and guards the buffer") {
    unsigned char                seed[8] = {};
    cinux::syscall::SyscallFrame frame   = plain_frame();
    frame.rdi                            = reinterpret_cast<unsigned long long>(seed);
    frame.rsi                            = sizeof(seed);
    ASSERT_EQ(cinux::syscall::HandleGetrandom(frame), 8);
    frame.rdi = 0;
    ASSERT_EQ(cinux::syscall::HandleGetrandom(frame), -14);
    frame.rdi = reinterpret_cast<unsigned long long>(seed);
    frame.rsi = 512;
    ASSERT_EQ(cinux::syscall::HandleGetrandom(frame), -22);
}

struct PreadPool {
    static const cinux::fs::FsOps kOps;
};

// NOLINTNEXTLINE(bugprone-throwing-static-initialization) constinit pins it
constinit const cinux::fs::FsOps PreadPool::kOps = [] noexcept {
    cinux::fs::FsOps ops{};
    ops.retain  = [](void*, cinux::fs::FsNode) {};
    ops.release = [](void*, cinux::fs::FsNode) {};
    ops.read    = [](void*, cinux::fs::FsNode, unsigned long offset, void* sink,
                     unsigned long count) -> cinux::base::Result<unsigned long> {
        static const unsigned char kBytes[8] = {'p', 'r', 'e', 'a', 'd', '6', '4', '!'};
        if (offset >= sizeof(kBytes)) {
            return {0UL};
        }
        const unsigned long kTake = std::min(count, sizeof(kBytes) - offset);
        cinux::base::CopyBytes(sink, kBytes + offset, kTake);
        return {kTake};
    };
    return ops;
}();

unsigned char g_pread_landed[8] = {};

unsigned long pread_cursor(const cinux::proc::Task* self, unsigned long seat) {
    return self->files->get(seat).value()->offset;
}

void pread_full_read(cinux::syscall::SyscallFrame& frame, unsigned long seat) {
    frame.rdi = seat;
    frame.rsi = reinterpret_cast<unsigned long long>(g_pread_landed);
    frame.rdx = 8;
    frame.r10 = 0;
    ASSERT_EQ(cinux::syscall::HandlePread(frame), 8);
    ASSERT_TRUE(g_pread_landed[0] == 'p');
    ASSERT_TRUE(g_pread_landed[7] == '!');
}

void pread_tail_and_refusals(cinux::syscall::SyscallFrame& frame) {
    frame.rsi = reinterpret_cast<unsigned long long>(g_pread_landed);
    frame.r10 = 6;
    ASSERT_EQ(cinux::syscall::HandlePread(frame), 2);
    ASSERT_TRUE(g_pread_landed[0] == '4');
    ASSERT_TRUE(g_pread_landed[1] == '!');
    frame.r10 = 0xFFFF'FFFF'FFFF'FFFFULL;
    ASSERT_EQ(cinux::syscall::HandlePread(frame), -22);
}

void run_pread_body() {
    cinux::syscall::SyscallFrame             frame = plain_frame();
    const cinux::proc::Task* const           kSelf = cinux::proc::Scheduler::self().current();
    const cinux::base::Result<unsigned long> kSeat =
        kSelf->files->alloc(&PreadPool::kOps, nullptr, {});
    ASSERT_TRUE(kSeat.ok());
    kSelf->files->get(kSeat.value()).value()->offset = 4;
    pread_full_read(frame, kSeat.value());
    ASSERT_TRUE(pread_cursor(kSelf, kSeat.value()) == 4);
    pread_tail_and_refusals(frame);
    ASSERT_TRUE(pread_cursor(kSelf, kSeat.value()) == 4);
    cinux::proc::Scheduler::self().exit_current();
}

TEST("syscall face: pread reads at a position and leaves the cursor") {
    FaceWorld world;
    world.run(run_pread_body);
}

void mmap_fixed_refusals(cinux::syscall::SyscallFrame& frame) {
    frame.rdi = 0x10000001;
    ASSERT_EQ(cinux::syscall::HandleMmap(frame), -22);
    frame.rdi = 4096;
    ASSERT_EQ(cinux::syscall::HandleMmap(frame), -22);
}

void run_mmap_fixed_body() {
    cinux::syscall::SyscallFrame frame = plain_frame();
    cinux::proc::Task* const     kSelf = cinux::proc::Scheduler::self().current();
    kSelf->vmas = cinux::base::Shared<cinux::mm::VmaStore>(new cinux::mm::VmaStore{});
    frame.rdx   = 3;
    frame.r10   = 0x32;
    frame.rdi   = 0x10000000;
    frame.rsi   = 8192;
    ASSERT_EQ(cinux::syscall::HandleMmap(frame), 0x10000000);
    frame.rsi = 4096;
    ASSERT_EQ(cinux::syscall::HandleMmap(frame), 0x10000000);
    ASSERT_TRUE(kSelf->vmas->find(0x10000000) != nullptr);
    ASSERT_TRUE(kSelf->vmas->find(0x10000000 + 4096) != nullptr);
    frame.rsi = 0x3000;
    ASSERT_EQ(cinux::syscall::HandleMmap(frame), 0x10000000);
    ASSERT_TRUE(kSelf->vmas->find(0x10000000) != nullptr);
    ASSERT_TRUE(kSelf->vmas->find(0x10000000 + 0x2000) != nullptr);
    ASSERT_TRUE(kSelf->vmas->find(0x10000000 + 0x3000) == nullptr);
    mmap_fixed_refusals(frame);
    cinux::proc::Scheduler::self().exit_current();
}

TEST("syscall face: fixed mappings replace what sat at the address") {
    FaceWorld world;
    world.run(run_mmap_fixed_body);
}

TEST("syscall face: sigtimedwait answers try-again without parking") {
    const cinux::syscall::SyscallFrame kFrame = plain_frame();
    ASSERT_EQ(cinux::syscall::HandleRtSigtimedwait(kFrame), -11);
}

struct TransferBackend {
    unsigned char                 source[8192]{};
    unsigned char                 sink[8192]{};
    unsigned long                 source_bytes = sizeof(source);
    unsigned long                 put_limit    = sizeof(sink);
    unsigned long                 fail_at      = ~0UL;
    bool                          would_block  = false;
    static const cinux::fs::FsOps kOps;
};

constexpr cinux::fs::FsOps TransferBackend::kOps = [] noexcept {
    cinux::fs::FsOps ops{};
    ops.retain  = [](void*, cinux::fs::FsNode) {};
    ops.release = [](void*, cinux::fs::FsNode) {};
    ops.stat    = [](void*, cinux::fs::FsNode node,
                     cinux::fs::FsStat& stat) -> cinux::base::Result<void> {
        stat      = {};
        stat.mode = node.ino == 2 ? 0010600 : 0100600;
        return {};
    };
    ops.read = [](void* self, cinux::fs::FsNode, unsigned long offset, void* sink,
                  unsigned long count) -> cinux::base::Result<unsigned long> {
        const auto& backend = *static_cast<TransferBackend*>(self);
        const auto  kEnd    = std::min(backend.source_bytes, sizeof(backend.source));
        if (offset >= kEnd) {
            return 0UL;
        }
        const auto kCount = std::min(count, kEnd - offset);
        cinux::base::CopyBytes(sink, backend.source + offset, kCount);
        return kCount;
    };
    ops.write = [](void* self, cinux::fs::FsNode, unsigned long offset, const void* source,
                   unsigned long count) -> cinux::base::Result<unsigned long> {
        const auto& backend = *static_cast<TransferBackend*>(self);
        if (backend.would_block) {
            return cinux::base::KernelError::kWouldBlock;
        }
        if (offset >= backend.fail_at) {
            return cinux::base::KernelError::kIOError;
        }
        if (offset >= sizeof(backend.sink)) {
            return 0UL;
        }
        const auto kCount = std::min({count, backend.put_limit, sizeof(backend.sink) - offset});
        cinux::base::CopyBytes(static_cast<TransferBackend*>(self)->sink + offset, source, kCount);
        return kCount;
    };
    return ops;
}();

void check_explicit_sendfile(const TransferBackend& backend, cinux::syscall::SyscallFrame& frame) {
    const auto* const kSelf = cinux::syscall::CurrentTask();
    const auto        kIn   = kSelf->files->get(frame.rsi).value();
    kIn->offset             = 9;
    long long offset        = 4;
    frame.rdx               = reinterpret_cast<unsigned long long>(&offset);
    frame.r10               = 37;
    ASSERT_EQ(cinux::syscall::HandleSendfile(frame), 37);
    ASSERT_EQ(offset, 41LL);
    ASSERT_EQ(kIn->offset, 9UL);
    ASSERT_EQ(backend.sink[0], backend.source[4]);
    frame.rdx = 0;
    frame.r10 = 7000;
    ASSERT_EQ(cinux::syscall::HandleSendfile(frame), 7000);
    ASSERT_EQ(kIn->offset, 7009UL);
}

void check_sendfile_short_and_faults(TransferBackend&              backend,
                                     cinux::syscall::SyscallFrame& frame) {
    const auto* const kSelf = cinux::syscall::CurrentTask();
    const auto        kIn   = kSelf->files->get(frame.rsi).value();
    const auto        kOut  = kSelf->files->get(frame.rdi).value();
    kIn->offset             = 0;
    kOut->offset            = 0;
    backend.put_limit       = 3;
    frame.rdx               = 0;
    frame.r10               = 10;
    ASSERT_EQ(cinux::syscall::HandleSendfile(frame), 3);
    ASSERT_EQ(kIn->offset, 3UL);
    ASSERT_EQ(kOut->offset, 3UL);
    backend.put_limit = sizeof(backend.sink);
    backend.fail_at   = 4096;
    kIn->offset       = 0;
    kOut->offset      = 0;
    frame.r10         = sizeof(backend.source);
    ASSERT_EQ(cinux::syscall::HandleSendfile(frame), 4096);
    ASSERT_EQ(kIn->offset, 4096UL);
    ASSERT_EQ(cinux::syscall::HandleSendfile(frame), -cinux::syscall::kEio);
}

void check_sendfile_refusals(TransferBackend& backend, cinux::syscall::SyscallFrame& frame) {
    const auto* const kSelf = cinux::syscall::CurrentTask();
    const auto        kOut  = kSelf->files->get(frame.rdi).value();
    kOut->status_flags      = cinux::syscall::kOpenWriteOnly | cinux::syscall::kOpenAppend;
    ASSERT_EQ(cinux::syscall::HandleSendfile(frame), -cinux::syscall::kEinval);
    kOut->status_flags  = cinux::syscall::kOpenWriteOnly | cinux::syscall::kOpenNonblock;
    backend.would_block = true;
    ASSERT_EQ(cinux::syscall::HandleSendfile(frame), -cinux::syscall::kEagain);
    frame.r10 = ~0ULL;
    ASSERT_EQ(cinux::syscall::HandleSendfile(frame), -cinux::syscall::kEinval);
    frame.rdx = cinux::syscall::kUserAddressLimit;
    ASSERT_EQ(cinux::syscall::HandleSendfile(frame), -cinux::syscall::kEfault);
}


void check_sendfile_bounds(cinux::syscall::SyscallFrame& frame) {
    const auto* const kSelf = cinux::syscall::CurrentTask();
    const auto        kIn   = kSelf->files->get(frame.rsi).value();
    kIn->offset             = 8192;
    frame.rdx               = 0;
    frame.r10               = 1;
    ASSERT_EQ(cinux::syscall::HandleSendfile(frame), 0);
    long long offset = -1;
    frame.rdx        = reinterpret_cast<unsigned long long>(&offset);
    ASSERT_EQ(cinux::syscall::HandleSendfile(frame), -cinux::syscall::kEinval);
    offset = std::numeric_limits<long long>::max();
    ASSERT_EQ(cinux::syscall::HandleSendfile(frame), -cinux::syscall::kEoverflow);
    frame.rdx = 0;
    frame.rdi = cinux::fs::kFileTableMax;
    ASSERT_EQ(cinux::syscall::HandleSendfile(frame), -cinux::syscall::kEbadf);
}

void check_sendfile_alias(cinux::syscall::SyscallFrame frame) {
    const auto kIn    = cinux::syscall::CurrentTask()->files->get(frame.rsi).value();
    kIn->status_flags = cinux::syscall::kOpenReadWrite;
    kIn->offset       = 2;
    frame.rdi         = frame.rsi;
    frame.rdx         = 0;
    frame.r10         = 5;
    ASSERT_EQ(cinux::syscall::HandleSendfile(frame), 5);
    ASSERT_EQ(kIn->offset, 7UL);
}

void run_sendfile_body() {
    TransferBackend backend;
    for (unsigned long index = 0; index < sizeof(backend.source); ++index) {
        backend.source[index] = static_cast<unsigned char>(index % 251);
    }
    const auto* const kSelf = cinux::syscall::CurrentTask();
    const auto        kIn   = kSelf->files->alloc(&TransferBackend::kOps, &backend, {});
    const auto        kOut  = kSelf->files->alloc(&TransferBackend::kOps, &backend, {},
                                                  {
                                                      .status = cinux::syscall::kOpenWriteOnly,
                                                  });
    ASSERT_TRUE(kIn.ok() && kOut.ok());
    cinux::syscall::SyscallFrame frame{};
    frame.rdi = kOut.value();
    frame.rsi = kIn.value();
    check_explicit_sendfile(backend, frame);
    check_sendfile_short_and_faults(backend, frame);
    check_sendfile_refusals(backend, frame);
    backend.would_block                                = false;
    backend.fail_at                                    = ~0UL;
    kSelf->files->get(frame.rdi).value()->status_flags = cinux::syscall::kOpenWriteOnly;
    kSelf->files->get(frame.rdi).value()->offset       = 0;
    check_sendfile_alias(frame);
    check_sendfile_bounds(frame);
    kSelf->files->close_all();
    cinux::proc::Scheduler::self().exit_current();
}

TEST("syscall face: sendfile keeps offsets and completed bytes across short writes and faults") {
    FaceWorld world;
    world.run(run_sendfile_body);
}


void check_close_range_marks(cinux::syscall::SyscallFrame& frame, unsigned int descriptor) {
    const auto* const kSelf = cinux::syscall::CurrentTask();
    ASSERT_TRUE(kSelf->files->set_cloexec(descriptor, false).ok());
    frame.rdi = descriptor;
    frame.rsi = descriptor;
    frame.rdx = 4;
    ASSERT_EQ(cinux::syscall::HandleCloseRange(frame), 0);
    ASSERT_TRUE(kSelf->files->cloexec(descriptor));
    frame.rdi = 9;
    frame.rsi = 3;
    ASSERT_EQ(cinux::syscall::HandleCloseRange(frame), -cinux::syscall::kEinval);
    frame.rdi = 0;
    frame.rsi = ~0U;
    frame.rdx = 0;
    ASSERT_EQ(cinux::syscall::HandleCloseRange(frame), 0);
}

void check_close_range_unshare(cinux::syscall::SyscallFrame& frame,
                               const unsigned int*           descriptors) {
    auto* const kSelf     = cinux::syscall::CurrentTask();
    const auto  kOriginal = kSelf->files;
    frame.rdi             = descriptors[0];
    frame.rsi             = ~0U;
    frame.rdx             = 2;
    ASSERT_EQ(cinux::syscall::HandleCloseRange(frame), 0);
    ASSERT_TRUE(!kSelf->files->get(descriptors[0]).ok());
    ASSERT_TRUE(!kSelf->files->get(descriptors[1]).ok());
    ASSERT_TRUE(kOriginal->get(descriptors[0]).ok());
    ASSERT_TRUE(kOriginal->get(descriptors[1]).ok());
    kSelf->files = kOriginal;
    check_close_range_marks(frame, descriptors[0]);
}

void run_pipe_flags_body() {
    const auto* const            kSelf = cinux::syscall::CurrentTask();
    unsigned int                 descriptors[2]{};
    cinux::syscall::SyscallFrame frame{};
    frame.rdi = reinterpret_cast<unsigned long long>(descriptors);
    frame.rsi = cinux::syscall::kOpenCloexec;
    ASSERT_EQ(cinux::syscall::HandlePipe2(frame), 0);
    ASSERT_TRUE(kSelf->files->cloexec(descriptors[0]));
    ASSERT_TRUE(kSelf->files->cloexec(descriptors[1]));
    ASSERT_EQ(kSelf->files->get(descriptors[1]).value()->status_flags,
              cinux::syscall::kOpenWriteOnly);
    check_close_range_unshare(frame, descriptors);
    cinux::proc::Scheduler::self().exit_current();
}

TEST("syscall face: pipe2 cloexec and close_range unshare preserve the other table") {
    FaceWorld world;
    world.run(run_pipe_flags_body);
}

void run_thread_signal_body() {
    auto* const kSelf = cinux::syscall::CurrentTask();
    kSelf->tgid       = kSelf->pid;
    cinux::syscall::SyscallFrame frame{};
    frame.rdi = kSelf->tgid;
    frame.rsi = kSelf->pid;
    ASSERT_EQ(cinux::syscall::HandleTgkill(frame), 0);
    frame.rdi += 1;
    ASSERT_EQ(cinux::syscall::HandleTgkill(frame), -cinux::syscall::kEsrch);
    frame.rdi = kSelf->tgid;
    frame.rsi = kSelf->pid + 1000;
    ASSERT_EQ(cinux::syscall::HandleTgkill(frame), -cinux::syscall::kEsrch);
    frame.rsi = kSelf->pid;
    frame.rdx = ~0ULL;
    ASSERT_EQ(cinux::syscall::HandleTgkill(frame), -cinux::syscall::kEinval);
    frame.rdi = kSelf->pid;
    frame.rsi = 0;
    ASSERT_EQ(cinux::syscall::HandleTkill(frame), 0);
    cinux::proc::Scheduler::self().exit_current();
}

TEST("syscall face: tgkill checks the target thread group and tkill checks only tid") {
    FaceWorld world;
    world.run(run_thread_signal_body);
}

}  // namespace

int main() {
    return cinux::test::RunAll();
}
