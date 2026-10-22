#include <string_view>

#include "../framework/framework.hpp"
#include "cinux/result.hpp"
#include "kernel/driver/device/device.hpp"
#include "kernel/driver/tty/console_tty.hpp"
#include "kernel/driver/tty/tty.hpp"
#include "kernel/driver/tty/tty_config.hpp"
#include "kernel/fs/devfs/devfs.hpp"
#include "kernel/fs/devfs/pty_node.hpp"
#include "kernel/fs/file_table.hpp"
#include "kernel/fs/open_file.hpp"
#include "kernel/fs/vfs.hpp"
#include "kernel/proc/sched_class.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/syscall/signal.hpp"
#include "kernel/syscall/sys_read.hpp"
#include "kernel/syscall/syscall.hpp"
#include "test_assert.hpp"

namespace cinux::driver {

Tty& ConsoleTty() {
    static Tty terminal;
    return terminal;
}

}  // namespace cinux::driver

namespace {

cinux::fs::DevFs           g_devices;
constexpr cinux::fs::FsOps kOps = cinux::fs::EraseFs<cinux::fs::DevFs>();
void (*g_body)()                = nullptr;

class ReadSink {
public:
    void switch_tasks(cinux::proc::Task& /*outgoing*/, cinux::proc::Task& /*incoming*/) { run(); }
    void enter_from_main(cinux::proc::Task& /*incoming*/) { run(); }
    void return_to_main(cinux::proc::Task& /*outgoing*/) {}
    void reclaim(cinux::proc::Task& /*dead*/) {}

private:
    static void run() {
        if (g_body != nullptr) {
            void (*const kBody)() = g_body;
            g_body                = nullptr;
            kBody();
        }
        cinux::proc::Scheduler::self().exit_current();
    }
};

struct PairWorld {
    cinux::fs::FsNode    master{};
    cinux::fs::FsNode    slave{};
    cinux::fs::FileTable files;

    PairWorld() {
        master = g_devices.adopt_open(g_devices.lookup("ptmx").value(), "ptmx").value();
        unsigned int number = 0;
        static_cast<void>(g_devices.ioctl(master, cinux::driver::kTtyTiocgptn,
                                          reinterpret_cast<unsigned long long>(&number)));
        char name[] = "pts/0";
        name[4]     = static_cast<char>('0' + number);
        slave       = g_devices.lookup(name).value();
        static_cast<void>(files.alloc(&kOps, &g_devices, master));
        static_cast<void>(files.alloc(&kOps, &g_devices, slave));
    }
};

void expect_master_eof(cinux::fs::FsNode master) {
    unsigned char heard = 0;
    ASSERT_TRUE(g_devices.readable(master));
    ASSERT_TRUE(g_devices.read(master, 0, &heard, 1).value() == 0UL);
    ASSERT_EQ(g_devices.write(master, 0, "x", 1).error(), cinux::base::KernelError::kIOError);
}

TEST("pty node: only the last independent slave open produces master EOF") {
    PairWorld  world;
    const auto kOther = world.files.alloc(&kOps, &g_devices, world.slave);
    ASSERT_TRUE(kOther.ok());
    ASSERT_TRUE(world.files.free(1).ok());
    unsigned char heard[8] = {};
    ASSERT_EQ(g_devices.read(world.master, 0, heard, sizeof(heard)).error(),
              cinux::base::KernelError::kWouldBlock);
    ASSERT_TRUE(g_devices.write(world.slave, 0, "tail", 4).value() == 4UL);
    ASSERT_TRUE(world.files.free(kOther.value()).ok());
    ASSERT_TRUE(g_devices.readable(world.master));
    ASSERT_TRUE(g_devices.read(world.master, 0, heard, sizeof(heard)).value() == 4UL);
    ASSERT_EQ(heard[0], static_cast<unsigned char>('t'));
    expect_master_eof(world.master);
}

TEST("pty node: duplicated and inherited descriptors retain the same device open") {
    PairWorld            world;
    cinux::fs::FileTable child;
    {
        const auto kHeld = world.files.shared(1);
        ASSERT_TRUE(kHeld.ok());
        ASSERT_TRUE(world.files.adopt(kHeld.value()).ok());
        ASSERT_TRUE(child.adopt(kHeld.value()).ok());
    }
    world.files.close_all();
    ASSERT_FALSE(g_devices.readable(world.master));
    child.close_all();
    ASSERT_TRUE(g_devices.readable(world.master));
}

TEST("pty node: master close drains slave input and rejects slave output") {
    PairWorld     world;
    unsigned char heard[8] = {};
    ASSERT_TRUE(g_devices.write(world.master, 0, "tail", 4).value() == 4UL);
    ASSERT_TRUE(world.files.free(0).ok());
    ASSERT_TRUE(g_devices.readable(world.slave));
    ASSERT_TRUE(g_devices.read(world.slave, 0, heard, sizeof(heard)).value() == 4UL);
    ASSERT_TRUE(g_devices.read(world.slave, 0, heard, sizeof(heard)).value() == 0UL);
    ASSERT_EQ(g_devices.write(world.slave, 0, "x", 1).error(), cinux::base::KernelError::kIOError);
}

TEST("pty node: a nonblocking canonical read preserves an unfinished line") {
    const PairWorld kWorld;
    unsigned char   heard[8] = {};
    ASSERT_TRUE(g_devices.write(kWorld.master, 0, "ab", 2).value() == 2UL);
    ASSERT_EQ(g_devices.read(kWorld.slave, 0, heard, sizeof(heard)).error(),
              cinux::base::KernelError::kWouldBlock);
    ASSERT_TRUE(g_devices.write(kWorld.master, 0, "\n", 1).value() == 1UL);
    ASSERT_TRUE(g_devices.read(kWorld.slave, 0, heard, sizeof(heard)).value() == 3UL);
    ASSERT_EQ(heard[0], static_cast<unsigned char>('a'));
    ASSERT_EQ(heard[1], static_cast<unsigned char>('b'));
    ASSERT_EQ(heard[2], static_cast<unsigned char>('\n'));
}

PairWorld* g_read_world = nullptr;

void read_nonblock_body() {
    auto&       world = *g_read_world;
    auto* const kTask = cinux::proc::Scheduler::self().current();
    {
        const auto kHeld = world.files.shared(0);
        ASSERT_TRUE(kHeld.ok());
        ASSERT_TRUE(kTask->files->adopt(kHeld.value()).ok());
    }
    auto* const kOpen                     = kTask->files->get(0).value();
    kOpen->status_flags                   = cinux::syscall::kOpenNonblock;
    unsigned char                heard[8] = {};
    cinux::syscall::SyscallFrame frame{};
    frame.rsi = reinterpret_cast<unsigned long long>(heard);
    frame.rdx = sizeof(heard);
    ASSERT_EQ(cinux::syscall::HandleRead(frame), -cinux::syscall::kEagain);
    frame.rdx = 0;
    ASSERT_EQ(cinux::syscall::HandleRead(frame), 0);
    frame.rdx           = sizeof(heard);
    kOpen->status_flags = 0;
    kTask->signals.pending =
        cinux::syscall::SigSet{1ULL << static_cast<unsigned int>(cinux::syscall::kSigChld - 1)};
    ASSERT_EQ(cinux::syscall::HandleRead(frame), -cinux::syscall::kEintr);
    kTask->signals.pending = {};
    ASSERT_TRUE(world.files.free(1).ok());
    ASSERT_EQ(cinux::syscall::HandleRead(frame), 0);
}

TEST("pty syscall: empty nonblocking master returns EAGAIN and hangup returns EOF") {
    PairWorld world;
    g_read_world = &world;
    ReadSink          sink;
    cinux::proc::Task reader;
    reader.name       = "reader";
    reader.stack_base = 0x80000;
    auto& scheduler   = cinux::proc::Scheduler::self();
    scheduler.init(sink);
    scheduler.install_class(cinux::proc::DefaultFifoClass());
    scheduler.seat(reader);
    g_body = read_nonblock_body;
    scheduler.run_until_done();
    g_read_world = nullptr;
}

TEST("pty node: process exit releases its inherited slave before the task is reaped") {
    PairWorld         world;
    ReadSink          sink;
    cinux::proc::Task child;
    child.name       = "slave owner";
    child.stack_base = 0x81000;
    {
        const auto kHeld = world.files.shared(1);
        ASSERT_TRUE(kHeld.ok());
        ASSERT_TRUE(child.files->adopt(kHeld.value()).ok());
    }
    ASSERT_TRUE(world.files.free(1).ok());
    ASSERT_FALSE(g_devices.readable(world.master));
    auto& scheduler = cinux::proc::Scheduler::self();
    scheduler.init(sink);
    scheduler.install_class(cinux::proc::DefaultFifoClass());
    scheduler.seat(child);
    scheduler.run_until_done();
    ASSERT_FALSE(child.files.holds());
    expect_master_eof(world.master);
}

}  // namespace

int main() {
    cinux::driver::DeviceModel::self().bind(g_devices);
    cinux::fs::RegisterPtyDevices();
    return cinux::test::RunAll();
}
