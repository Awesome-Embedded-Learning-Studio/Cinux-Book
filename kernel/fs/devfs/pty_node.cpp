#include "kernel/fs/devfs/pty_node.hpp"

#include <string_view>

#include "cinux/assert.hpp"
#include "cinux/memory.hpp"
#include "cinux/result.hpp"
#include "kernel/driver/tty/console_tty.hpp"
#include "kernel/driver/tty/pty.hpp"
#include "kernel/driver/tty/pty_config.hpp"
#include "kernel/driver/tty/tty.hpp"
#include "kernel/driver/tty/tty_config.hpp"
#include "kernel/fs/devfs/devfs.hpp"
#include "kernel/fs/vfs.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/signals.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::fs {

namespace {

constexpr unsigned long kPgrpBytes = 4;

constexpr unsigned long kWinsizeBytes = 8;

/// One seat in the pair table: the pair itself and the two faces.
struct Seat {
    bool               used            = false;
    driver::Pty*       pair            = nullptr;
    DeviceOps          master_ops      = {};
    DeviceOps          slave_ops       = {};
    unsigned long long foreground_pgid = 0;
};

Seat g_seats[driver::kPtyPairs];

/// The table the resolvers answer from, seated at registration.
DevFs* g_table = nullptr;

void pty_wait(void* /*context*/) {
    proc::Scheduler::self().yield();
}

Seat* seat_of(void* context) {
    return static_cast<Seat*>(context);
}

base::Result<unsigned long> master_read(void* context, void* sink, unsigned long count) {
    const Seat* const kSeat = seat_of(context);
    while (!kSeat->pair->master_has_input()) {
        pty_wait(nullptr);
    }
    return kSeat->pair->master_read(sink, count);
}

base::Result<unsigned long> master_write(void* context, const void* source, unsigned long count) {
    return seat_of(context)->pair->master_write(source, count);
}

bool master_has_input(void* context) {
    return seat_of(context)->pair->master_has_input();
}

long long master_ioctl(void* context, unsigned long long word, unsigned long long argument) {
    if (word == driver::kTtyTiocgptn) {
        const Seat* const kSeat = seat_of(context);
        for (unsigned long index = 0; index < driver::kPtyPairs; ++index) {
            if (&g_seats[index] == kSeat) {
                const auto  kIndex = static_cast<unsigned int>(index);
                // NOLINTNEXTLINE(performance-no-int-to-ptr)
                auto* const kWhere = reinterpret_cast<unsigned int*>(argument);
                base::CopyBytes(kWhere, &kIndex, sizeof(kIndex));
                return 0;
            }
        }
    }
    if (word == driver::kTtyTiocsptlck) {
        return 0;
    }
    return -syscall::kEnotty;
}

base::Result<unsigned long> slave_read(void* context, void* sink, unsigned long count) {
    const Seat* const   kSeat   = seat_of(context);
    driver::Tty&        tty     = kSeat->pair->slave();
    const unsigned long kGot    = tty.read_line(static_cast<unsigned char*>(sink), count);
    const int           kSignal = tty.take_pending_signal();
    if (kSignal != 0 && kSeat->foreground_pgid != 0) {
        proc::SendToGroup(kSeat->foreground_pgid, kSignal);
    }
    if (kSignal != 0) {
        return base::KernelError::kInterrupted;
    }
    return kGot;
}

base::Result<unsigned long> slave_write(void* context, const void* source, unsigned long count) {
    return seat_of(context)->pair->slave_write(source, count);
}

bool slave_has_input(void* context) {
    const Seat* const kSeat = seat_of(context);
    return kSeat->pair->raw_has_keys() || kSeat->pair->slave().has_line();
}

long long slave_ioctl(void* context, unsigned long long word, unsigned long long argument) {
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto* const  kOut  = reinterpret_cast<unsigned char*>(argument);
    Seat* const  kSeat = seat_of(context);
    driver::Tty& tty   = kSeat->pair->slave();
    if (word == driver::kTtyTcgets) {
        tty.copy_termios(kOut);
        return 0;
    }
    if (word == driver::kTtyTcsets || word == driver::kTtyTcsetsw || word == driver::kTtyTcsetsf) {
        tty.apply_termios(kOut);
        return 0;
    }
    if (word == driver::kTtyTiocgwinsz) {
        const driver::TtyWindow kShape = tty.window();
        base::CopyBytes(kOut, &kShape, kWinsizeBytes);
        return 0;
    }
    if (word == driver::kTtyTiocgpgrp) {
        base::CopyBytes(kOut, &kSeat->foreground_pgid, kPgrpBytes);
        return 0;
    }
    if (word == driver::kTtyTiocspgrp) {
        unsigned int pgid = 0;
        base::CopyBytes(&pgid, kOut, kPgrpBytes);
        kSeat->foreground_pgid = pgid;
        return 0;
    }
    if (word == driver::kTtyTiocsctty) {
        proc::Task* const kSelf   = cinux::syscall::CurrentTask();
        const bool        kLeader = kSelf->sid == kSelf->pid;
        if (!kLeader || kSelf->controlling_tty != nullptr) {
            return -syscall::kEperm;
        }
        kSelf->controlling_tty = &tty;
        kSeat->foreground_pgid = kSelf->pgid;
        return 0;
    }
    return -syscall::kEnotty;
}

base::Result<unsigned long> ptmx_read(void* /*context*/, void* /*sink*/, unsigned long /*count*/) {
    return base::KernelError::kIOError;
}

base::Result<unsigned long> ptmx_write(void* /*context*/, const void* /*source*/,
                                       unsigned long /*count*/) {
    return base::KernelError::kIOError;
}

const DeviceOps kPtmxOps{
    .read      = ptmx_read,
    .write     = ptmx_write,
    .has_input = nullptr,
    .ioctl     = nullptr,
    .release   = nullptr,
    .context   = nullptr,
};

/// Mints a pair in the first never-used seat and hands back its master
/// node. Seats are not recycled yet: telling a freed seat from a live
/// one needs the pair's own lifetime bookkeeping — whose descriptors
/// still point here, whether the slave hangs up — and guessing wrong
/// lets an old slave write into a new master's pair. Until that
/// bookkeeping exists, the eighth pair and every one after answer
/// ENOMEM honestly.
base::Result<FsNode> mint_pair() {
    for (unsigned long index = 0; index < driver::kPtyPairs; ++index) {
        Seat& seat = g_seats[index];
        if (seat.used) {
            continue;
        }
        seat.pair                 = new driver::Pty(&pty_wait);
        seat.used                 = true;
        seat.master_ops.read      = master_read;
        seat.master_ops.write     = master_write;
        seat.master_ops.has_input = master_has_input;
        seat.master_ops.ioctl     = master_ioctl;
        seat.master_ops.release   = nullptr;
        seat.master_ops.context   = &seat;
        seat.slave_ops.read       = slave_read;
        seat.slave_ops.write      = slave_write;
        seat.slave_ops.has_input  = slave_has_input;
        seat.slave_ops.ioctl      = slave_ioctl;
        seat.slave_ops.release    = nullptr;
        seat.slave_ops.context    = &seat;
        seat.pair->slave().set_window(driver::ConsoleTty().window());
        return FsNode{
            .opaque = &seat.master_ops,
            .type   = InodeType::kCharDevice,
            .ino    = driver::kPtyInoBase + (index * 2UL) + 1UL,
            .size   = 0,
        };
    }
    return base::KernelError::kOutOfMemory;
}

/// The slave node of a live seat, or NotFound when the seat is free.
base::Result<FsNode> slave_node_of(const Seat& seat, unsigned long index) {
    if (!seat.used) {
        return base::KernelError::kNotFound;
    }
    return FsNode{
        .opaque = &const_cast<Seat&>(seat).slave_ops,
        .type   = InodeType::kCharDevice,
        .ino    = driver::kPtyInoBase + (index * 2UL) + 2UL,
        .size   = 0,
    };
}

/// Parses "pts/<N>" into a live slave node, and "tty" into the calling
/// session's own controlling terminal, the dynamic half of the table's
/// namespace.
base::Result<FsNode> resolve_dynamic(std::string_view path) {
    constexpr std::string_view kPrefix = "pts/";
    if (path == "tty") {
        const proc::Task* const kSelf = cinux::syscall::CurrentTask();
        if (kSelf->controlling_tty == nullptr || g_table == nullptr) {
            return base::KernelError::kNotFound;
        }
        if (kSelf->controlling_tty == &driver::ConsoleTty()) {
            return g_table->lookup("console");
        }
        for (unsigned long index = 0; index < driver::kPtyPairs; ++index) {
            if (kSelf->controlling_tty == &g_seats[index].pair->slave()) {
                return slave_node_of(g_seats[index], index);
            }
        }
        return base::KernelError::kNotFound;
    }
    if (!path.starts_with(kPrefix)) {
        return base::KernelError::kNotFound;
    }
    const std::string_view kDigits = path.substr(kPrefix.size());
    if (kDigits.empty() || kDigits.size() > 2) {
        return base::KernelError::kNotFound;
    }
    unsigned long index = 0;
    for (const char kDigit : kDigits) {
        if (kDigit < '0' || kDigit > '9') {
            return base::KernelError::kNotFound;
        }
        index = (index * 10) + static_cast<unsigned long>(kDigit - '0');
    }
    if (index >= driver::kPtyPairs) {
        return base::KernelError::kNotFound;
    }
    return slave_node_of(g_seats[index], index);
}

/// Hands opening ptmx a fresh pair's master, and every other path itself.
base::Result<FsNode> adopt_open(FsNode node, std::string_view path) {
    if (path == "ptmx") {
        return mint_pair();
    }
    return node;
}

}  // namespace

void RegisterPtyDevices(DevFs& devfs) {
    const base::Result<void> kPtmx = devfs.register_device("ptmx", driver::kPtmxDeviceId, kPtmxOps);
    base::safety::Check(kPtmx.ok(), "devfs refused the ptmx seat");
    g_table = &devfs;
    devfs.set_dynamic_lookup(&resolve_dynamic);
    devfs.set_open_adopter(&adopt_open);
}

}  // namespace cinux::fs
