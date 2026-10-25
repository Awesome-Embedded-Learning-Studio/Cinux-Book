#include "kernel/fs/devfs/pty_node.hpp"

#include <string_view>

#include "cinux/assert.hpp"
#include "cinux/container/name_table.hpp"
#include "cinux/memory.hpp"
#include "cinux/parse.hpp"
#include "cinux/result.hpp"
#include "cinux/singleton.hpp"
#include "kernel/driver/device/device.hpp"
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

/// One seat in the pair table: the pair itself and the two faces.
struct Seat {
    bool               used            = false;
    driver::Pty*       pair            = nullptr;
    DeviceOps          master_ops      = {};
    DeviceOps          slave_ops       = {};
    unsigned long long foreground_pgid = 0;
};

/// The slave node of a live seat, or NotFound when the seat is free.
base::Result<FsNode> slave_node_of(Seat& seat, unsigned long index) {
    if (!seat.used) {
        return base::KernelError::kNotFound;
    }
    return FsNode{
        .opaque = &seat.slave_ops,
        .type   = InodeType::kCharDevice,
        .ino    = driver::kPtyInoBase + (index * 2UL) + 2UL,
        .size   = 0,
    };
}

void pty_wait(void* /*context*/) {
    proc::Scheduler::self().yield();
}

Seat* seat_of(void* context) {
    return static_cast<Seat*>(context);
}

base::Result<unsigned long> master_read(void* context, void* sink, unsigned long count) {
    const Seat* const   kSeat = seat_of(context);
    const unsigned long kGot  = kSeat->pair->master_read(sink, count);
    if (count != 0 && kGot == 0 && !kSeat->pair->slave_closed()) {
        return base::KernelError::kWouldBlock;
    }
    return kGot;
}

base::Result<unsigned long> master_write(void* context, const void* source, unsigned long count) {
    driver::Pty& pair = *seat_of(context)->pair;
    if (count != 0 && pair.slave_closed()) {
        return base::KernelError::kIOError;
    }
    return pair.master_write(source, count);
}

bool master_has_input(void* context) {
    return seat_of(context)->pair->master_has_input();
}

base::Result<unsigned long> slave_read(void* context, void* sink, unsigned long count) {
    const Seat* const   kSeat   = seat_of(context);
    driver::Tty&        tty     = kSeat->pair->slave();
    const unsigned long kGot    = tty.read_line(static_cast<unsigned char*>(sink), count, false);
    const int           kSignal = tty.take_pending_signal();
    if (kSignal != 0 && kSeat->foreground_pgid != 0) {
        proc::SendToGroup(kSeat->foreground_pgid, kSignal);
    }
    if (kSignal != 0) {
        return base::KernelError::kInterrupted;
    }
    if (count != 0 && kGot == 0 && !tty.hung_up()) {
        return base::KernelError::kWouldBlock;
    }
    return kGot;
}

base::Result<unsigned long> slave_write(void* context, const void* source, unsigned long count) {
    driver::Pty& pair = *seat_of(context)->pair;
    if (count != 0 && pair.master_closed()) {
        return base::KernelError::kIOError;
    }
    return pair.slave_write(source, count);
}

bool slave_has_input(void* context) {
    const Seat* const kSeat = seat_of(context);
    return kSeat->pair->slave().has_input();
}

void master_retain(void* context) {
    seat_of(context)->pair->retain_master();
}

void master_release(void* context) {
    seat_of(context)->pair->release_master();
}

void slave_retain(void* context) {
    seat_of(context)->pair->retain_slave();
}

void slave_release(void* context) {
    seat_of(context)->pair->release_slave();
}

long long master_answer_number(const Seat& seat, unsigned long long argument);

long long master_answer_lock(const Seat& seat, unsigned long long argument);

/// One master question and the seat that answers it.
struct MasterAnswer {
    unsigned long long word;
    long long (*answer)(const Seat& seat, unsigned long long argument);
};

constexpr MasterAnswer kMasterAnswers[] = {
    {.word = driver::kTtyTiocgptn, .answer = master_answer_number},
    {.word = driver::kTtyTiocsptlck, .answer = master_answer_lock},
};

long long master_ioctl(void* context, unsigned long long word, unsigned long long argument) {
    const MasterAnswer* const kAnswer = cinux::base::container::FindByWord(kMasterAnswers, word);
    if (kAnswer == nullptr) {
        return -syscall::kEnotty;
    }
    return kAnswer->answer(*seat_of(context), argument);
}

template <unsigned long Bytes = driver::kTermiosBytes>
long long slave_answer_termios_get(const Seat& seat, unsigned long long argument) {
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto* const kOut = reinterpret_cast<unsigned char*>(argument);
    seat.pair->slave().copy_termios(kOut, Bytes);
    return 0;
}

template <unsigned long Bytes = driver::kTermiosBytes>
long long slave_answer_termios_set(const Seat& seat, unsigned long long argument) {
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto const* const kOut = reinterpret_cast<const unsigned char*>(argument);
    seat.pair->slave().apply_termios(kOut, Bytes);
    return 0;
}

long long slave_answer_window(const Seat& seat, unsigned long long argument) {
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto* const             kOut   = reinterpret_cast<unsigned char*>(argument);
    const driver::TtyWindow kShape = seat.pair->slave().window();
    base::CopyBytes(kOut, &kShape, driver::kTtyWinsizeBytes);
    return 0;
}

long long slave_answer_pgrp_get(const Seat& seat, unsigned long long argument) {
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto* const kOut = reinterpret_cast<unsigned char*>(argument);
    base::CopyBytes(kOut, &seat.foreground_pgid, driver::kTtyPgrpBytes);
    return 0;
}

/// One slave question the seat can answer without changing it.
struct SlaveAnswer {
    unsigned long long word;
    long long (*answer)(const Seat& seat, unsigned long long argument);
};

constexpr SlaveAnswer kSlaveAnswers[] = {
    {.word = driver::kTtyTcgets, .answer = slave_answer_termios_get<>},
    {.word = driver::kTtyTcsets, .answer = slave_answer_termios_set<>},
    {.word = driver::kTtyTcsetsw, .answer = slave_answer_termios_set<>},
    {.word = driver::kTtyTcsetsf, .answer = slave_answer_termios_set<>},
    {.word = driver::kTtyTcgets2, .answer = slave_answer_termios_get<driver::kTermios2Bytes>},
    {.word = driver::kTtyTcsets2, .answer = slave_answer_termios_set<driver::kTermios2Bytes>},
    {.word = driver::kTtyTcsetsw2, .answer = slave_answer_termios_set<driver::kTermios2Bytes>},
    {.word = driver::kTtyTcsetsf2, .answer = slave_answer_termios_set<driver::kTermios2Bytes>},
    {.word = driver::kTtyTiocgwinsz, .answer = slave_answer_window},
    {.word = driver::kTtyTiocgpgrp, .answer = slave_answer_pgrp_get},
};

long long slave_answer_pgrp_set(Seat& seat, unsigned long long argument) {
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto const* const kOut = reinterpret_cast<const unsigned char*>(argument);
    unsigned int      pgid = 0;
    base::CopyBytes(&pgid, kOut, driver::kTtyPgrpBytes);
    seat.foreground_pgid = pgid;
    return 0;
}

long long slave_answer_ctty(Seat& seat, unsigned long long /*argument*/) {
    proc::Task* const kSelf   = cinux::syscall::CurrentTask();
    const bool        kLeader = kSelf->sid == kSelf->pid;
    if (!kLeader || kSelf->controlling_tty != nullptr) {
        return -syscall::kEperm;
    }
    kSelf->controlling_tty = &seat.pair->slave();
    seat.foreground_pgid   = kSelf->pgid;
    return 0;
}

/// One slave question the seat answers by changing it.
struct SlaveChangeAnswer {
    unsigned long long word;
    long long (*answer)(Seat& seat, unsigned long long argument);
};

constexpr SlaveChangeAnswer kSlaveChangeAnswers[] = {
    {.word = driver::kTtyTiocspgrp, .answer = slave_answer_pgrp_set},
    {.word = driver::kTtyTiocsctty, .answer = slave_answer_ctty},
};

long long slave_ioctl(void* context, unsigned long long word, unsigned long long argument) {
    const SlaveAnswer* const kAnswer = cinux::base::container::FindByWord(kSlaveAnswers, word);
    if (kAnswer != nullptr) {
        return kAnswer->answer(*seat_of(context), argument);
    }
    const SlaveChangeAnswer* const kChange =
        cinux::base::container::FindByWord(kSlaveChangeAnswers, word);
    if (kChange != nullptr) {
        return kChange->answer(*seat_of(context), argument);
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

/// The pair table, one registry behind the devfs resolvers.
class PtyTable : public cinux::base::Singleton<PtyTable> {
    friend class cinux::base::Singleton<PtyTable>;

public:
    /**
     * @brief         Where one seat lives in the table.
     * @param[in]     seat   The seat being asked about.
     * @return        Its index, or kPtyPairs when it is no seat of ours.
     * @since         0.6.0
     * @ingroup       kernel_fs
     */
    [[nodiscard]] unsigned long index_of(const Seat* seat) const {
        for (unsigned long index = 0; index < driver::kPtyPairs; ++index) {
            if (&seats_[index] == seat) {
                return index;
            }
        }
        return driver::kPtyPairs;
    }

    /// Mints a pair in the first never-used seat and hands back its
    /// master node. Seats are not recycled yet: telling a freed seat
    /// from a live one needs the pair's own lifetime bookkeeping —
    /// whose descriptors still point here, whether the slave hangs up —
    /// and guessing wrong lets an old slave write into a new master's
    /// pair. Until that bookkeeping exists, the eighth pair and every
    /// one after answer ENOMEM honestly.
    [[nodiscard]] base::Result<FsNode> mint_pair() {
        for (unsigned long index = 0; index < driver::kPtyPairs; ++index) {
            Seat& seat = seats_[index];
            if (seat.used) {
                continue;
            }
            seat.pair                 = new driver::Pty(&pty_wait);
            seat.used                 = true;
            seat.master_ops.read      = master_read;
            seat.master_ops.write     = master_write;
            seat.master_ops.has_input = master_has_input;
            seat.master_ops.ioctl     = master_ioctl;
            seat.master_ops.release   = master_release;
            seat.master_ops.context   = &seat;
            seat.master_ops.retain    = master_retain;
            seat.slave_ops.read       = slave_read;
            seat.slave_ops.write      = slave_write;
            seat.slave_ops.has_input  = slave_has_input;
            seat.slave_ops.ioctl      = slave_ioctl;
            seat.slave_ops.release    = slave_release;
            seat.slave_ops.context    = &seat;
            seat.slave_ops.retain     = slave_retain;
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

    /// Parses "pts/<N>" into a live slave node, and "tty" into the
    /// calling session's own controlling terminal, the dynamic half of
    /// the table's namespace.
    // NOLINTNEXTLINE(readability-function-size) the resolver names three shapes
    [[nodiscard]] base::Result<FsNode> resolve(std::string_view path) {
        constexpr std::string_view kPrefix = "pts/";
        if (path == "tty") {
            const proc::Task* const kSelf = cinux::syscall::CurrentTask();
            if (kSelf->controlling_tty == nullptr) {
                return base::KernelError::kNotFound;
            }
            if (kSelf->controlling_tty == &driver::ConsoleTty()) {
                return driver::DeviceModel::self().bound_devfs().lookup("console");
            }
            for (unsigned long index = 0; index < driver::kPtyPairs; ++index) {
                if (!seats_[index].used) {
                    continue;
                }
                if (kSelf->controlling_tty == &seats_[index].pair->slave()) {
                    return slave_node_of(seats_[index], index);
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
        const base::Result<unsigned long> kIndex = base::ParseDecimal(kDigits);
        if (!kIndex.ok() || kIndex.value() >= driver::kPtyPairs) {
            return base::KernelError::kNotFound;
        }
        return slave_node_of(seats_[kIndex.value()], kIndex.value());
    }

    /// Hands opening ptmx a fresh pair's master, and every other path
    /// itself.
    [[nodiscard]] base::Result<FsNode> adopt(FsNode node, std::string_view path) {
        if (path == "ptmx") {
            return mint_pair();
        }
        return node;
    }

private:
    PtyTable() = default;

    Seat seats_[driver::kPtyPairs] = {};
};

long long master_answer_number(const Seat& seat, unsigned long long argument) {
    const unsigned long kIndex = PtyTable::self().index_of(&seat);
    if (kIndex == driver::kPtyPairs) {
        return -syscall::kEnotty;
    }
    const auto  kNumber = static_cast<unsigned int>(kIndex);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto* const kWhere  = reinterpret_cast<unsigned int*>(argument);
    base::CopyBytes(kWhere, &kNumber, sizeof(kNumber));
    return 0;
}

long long master_answer_lock(const Seat& /*seat*/, unsigned long long /*argument*/) {
    return 0;
}

/// The devfs-facing resolver, a function pointer over the one table.
base::Result<FsNode> resolve_dynamic(std::string_view path) {
    return PtyTable::self().resolve(path);
}

/// The devfs-facing open exchanger, a function pointer over the table.
base::Result<FsNode> adopt_open(FsNode node, std::string_view path) {
    return PtyTable::self().adopt(node, path);
}

}  // namespace

void RegisterPtyDevices() {
    const base::Result<void> kPtmx = driver::DeviceModel::self().seat({
        .klass       = driver::DeviceClass::kCharacter,
        .name        = "ptmx",
        .number      = driver::kPtmxDeviceId,
        .ops         = &kPtmxOps,
        .bus         = "virtual",
        .driver_name = "pty",
    });
    base::safety::Check(kPtmx.ok(), "device model refused the ptmx seat");
    driver::DeviceModel::self().bound_devfs().set_dynamic_lookup(&resolve_dynamic);
    driver::DeviceModel::self().bound_devfs().set_open_adopter(&adopt_open);
}

}  // namespace cinux::fs
