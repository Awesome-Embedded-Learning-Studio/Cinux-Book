#include "kernel/fs/devfs/console_node.hpp"

#include "cinux/assert.hpp"
#include "cinux/memory.hpp"
#include "cinux/result.hpp"
#include "kernel/console/console.hpp"
#include "kernel/driver/tty/console_tty.hpp"
#include "kernel/driver/tty/tty.hpp"
#include "kernel/driver/tty/tty_config.hpp"
#include "kernel/fs/devfs/devfs.hpp"
#include "kernel/proc/signals.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::fs {

namespace {

constexpr unsigned long kWinsizeBytes = 8;
constexpr unsigned long kPgrpBytes    = 4;

/// The console's foreground group: whose keys these are, whose ^C lands.
unsigned long long g_foreground_pgid = 1;

base::Result<unsigned long> console_read(void* /*context*/, void* sink, unsigned long count) {
    driver::Tty&        tty     = driver::ConsoleTty();
    const unsigned long kGot    = tty.read_line(static_cast<unsigned char*>(sink), count);
    const int           kSignal = tty.take_pending_signal();
    if (kSignal != 0 && g_foreground_pgid != 0) {
        proc::SendToGroup(g_foreground_pgid, kSignal);
    }
    if (kSignal != 0) {
        return base::KernelError::kInterrupted;
    }
    return kGot;
}

base::Result<unsigned long> console_write(void* /*context*/, const void* source,
                                          unsigned long count) {
    auto const* const kIn = static_cast<const unsigned char*>(source);
    for (unsigned long index = 0; index < count; ++index) {
        console::PutChar(static_cast<char>(kIn[index]));
    }
    return count;
}

bool console_has_input(void* /*context*/) {
    return driver::ConsoleTty().has_input();
}

/// Answers the terminal questions the console node understands.
long long console_ioctl(void* /*context*/, unsigned long long word, unsigned long long argument) {
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto* const kOut = reinterpret_cast<unsigned char*>(argument);
    if (word == driver::kTtyTcgets) {
        driver::ConsoleTty().copy_termios(kOut);
        return 0;
    }
    if (word == driver::kTtyTcsets || word == driver::kTtyTcsetsw || word == driver::kTtyTcsetsf) {
        driver::ConsoleTty().apply_termios(kOut);
        return 0;
    }
    if (word == driver::kTtyTiocgwinsz) {
        const driver::TtyWindow kShape = driver::ConsoleTty().window();
        base::CopyBytes(kOut, &kShape, kWinsizeBytes);
        return 0;
    }
    if (word == driver::kTtyTiocgpgrp) {
        base::CopyBytes(kOut, &g_foreground_pgid, kPgrpBytes);
        return 0;
    }
    if (word == driver::kTtyTiocspgrp) {
        unsigned int pgid = 0;
        base::CopyBytes(&pgid, kOut, kPgrpBytes);
        g_foreground_pgid = pgid;
        return 0;
    }
    if (word == driver::kTtyTiocsctty) {
        proc::Task* const kSelf   = cinux::syscall::CurrentTask();
        const bool        kLeader = kSelf->sid == kSelf->pid;
        if (!kLeader || kSelf->controlling_tty != nullptr) {
            return -syscall::kEperm;
        }
        kSelf->controlling_tty = &driver::ConsoleTty();
        g_foreground_pgid      = kSelf->pgid;
        return 0;
    }
    return -syscall::kEnotty;
}

const DeviceOps kConsoleOps{
    .read      = console_read,
    .write     = console_write,
    .has_input = console_has_input,
    .ioctl     = console_ioctl,
    .release   = nullptr,
    .context   = nullptr,
};

}  // namespace

void RegisterConsoleDevice(DevFs& devfs) {
    const base::Result<void> kSeated =
        devfs.register_device("console", MakeDeviceId(5, 1), kConsoleOps);
    base::safety::Check(kSeated.ok(), "devfs refused the console seat");
}

void AdoptConsoleForeground(unsigned long long pgid) {
    g_foreground_pgid = pgid;
}

}  // namespace cinux::fs
