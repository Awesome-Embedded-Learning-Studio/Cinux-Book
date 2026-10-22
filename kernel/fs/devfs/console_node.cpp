#include "kernel/fs/devfs/console_node.hpp"

#include "cinux/assert.hpp"
#include "cinux/container/name_table.hpp"
#include "cinux/memory.hpp"
#include "cinux/result.hpp"
#include "cinux/singleton.hpp"
#include "kernel/console/console.hpp"
#include "kernel/driver/device/device.hpp"
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

/// The console's foreground group: whose keys these are, whose ^C lands.
class ConsoleForeground : public cinux::base::Singleton<ConsoleForeground> {
    friend class cinux::base::Singleton<ConsoleForeground>;

public:
    /**
     * @brief         The group holding the console's keys.
     * @return        The foreground pgid, zero when nobody holds them.
     * @since         0.6.0
     * @ingroup       kernel_fs
     */
    [[nodiscard]] unsigned long long get() const { return pgid_; }

    /**
     * @brief         Hands the console's keys to a group.
     * @param[in,out] pgid   The group taking the foreground.
     * @return        None
     * @since         0.6.0
     * @ingroup       kernel_fs
     */
    void adopt(unsigned long long pgid) { pgid_ = pgid; }

private:
    ConsoleForeground() = default;

    unsigned long long pgid_ = 1;
};

base::Result<unsigned long> console_read(void* /*context*/, void* sink, unsigned long count) {
    driver::Tty&             tty     = driver::ConsoleTty();
    const unsigned long      kGot    = tty.read_line(static_cast<unsigned char*>(sink), count);
    const int                kSignal = tty.take_pending_signal();
    const unsigned long long kGroup  = ConsoleForeground::self().get();
    if (kSignal != 0 && kGroup != 0) {
        proc::SendToGroup(kGroup, kSignal);
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

long long answer_termios_get(unsigned long long argument) {
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto* const kOut = reinterpret_cast<unsigned char*>(argument);
    driver::ConsoleTty().copy_termios(kOut);
    return 0;
}

long long answer_termios_set(unsigned long long argument) {
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto const* const kOut = reinterpret_cast<const unsigned char*>(argument);
    driver::ConsoleTty().apply_termios(kOut);
    return 0;
}

long long answer_window(unsigned long long argument) {
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto* const             kOut   = reinterpret_cast<unsigned char*>(argument);
    const driver::TtyWindow kShape = driver::ConsoleTty().window();
    base::CopyBytes(kOut, &kShape, driver::kTtyWinsizeBytes);
    return 0;
}

long long answer_pgrp_get(unsigned long long argument) {
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto* const              kOut   = reinterpret_cast<unsigned char*>(argument);
    const unsigned long long kGroup = ConsoleForeground::self().get();
    base::CopyBytes(kOut, &kGroup, driver::kTtyPgrpBytes);
    return 0;
}

long long answer_pgrp_set(unsigned long long argument) {
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto const* const kOut = reinterpret_cast<const unsigned char*>(argument);
    unsigned int      pgid = 0;
    base::CopyBytes(&pgid, kOut, driver::kTtyPgrpBytes);
    ConsoleForeground::self().adopt(pgid);
    return 0;
}

long long answer_ctty(unsigned long long /*argument*/) {
    proc::Task* const kSelf   = cinux::syscall::CurrentTask();
    const bool        kLeader = kSelf->sid == kSelf->pid;
    if (!kLeader || kSelf->controlling_tty != nullptr) {
        return -syscall::kEperm;
    }
    kSelf->controlling_tty = &driver::ConsoleTty();
    ConsoleForeground::self().adopt(kSelf->pgid);
    return 0;
}

/// One console question and its answer.
struct ConsoleAnswer {
    unsigned long long word;
    long long (*answer)(unsigned long long argument);
};

constexpr ConsoleAnswer kConsoleAnswers[] = {
    {.word = driver::kTtyTcgets, .answer = answer_termios_get},
    {.word = driver::kTtyTcsets, .answer = answer_termios_set},
    {.word = driver::kTtyTcsetsw, .answer = answer_termios_set},
    {.word = driver::kTtyTcsetsf, .answer = answer_termios_set},
    {.word = driver::kTtyTiocgwinsz, .answer = answer_window},
    {.word = driver::kTtyTiocgpgrp, .answer = answer_pgrp_get},
    {.word = driver::kTtyTiocspgrp, .answer = answer_pgrp_set},
    {.word = driver::kTtyTiocsctty, .answer = answer_ctty},
};

/// Answers the terminal questions the console node understands.
long long console_ioctl(void* /*context*/, unsigned long long word, unsigned long long argument) {
    const ConsoleAnswer* const kAnswer = cinux::base::container::FindByWord(kConsoleAnswers, word);
    if (kAnswer == nullptr) {
        return -syscall::kEnotty;
    }
    return kAnswer->answer(argument);
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

void RegisterConsoleDevice() {
    const base::Result<void> kSeated = driver::DeviceModel::self().seat({
        .klass       = driver::DeviceClass::kCharacter,
        .name        = "console",
        .number      = MakeDeviceId(5, 1),
        .ops         = &kConsoleOps,
        .bus         = "virtual",
        .driver_name = "console",
    });
    base::safety::Check(kSeated.ok(), "device model refused the console seat");
}

void AdoptConsoleForeground(unsigned long long pgid) {
    ConsoleForeground::self().adopt(pgid);
}

}  // namespace cinux::fs
