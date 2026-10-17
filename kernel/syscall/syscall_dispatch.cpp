/**
 * @file    syscall_dispatch.cpp
 * @brief   The numbered door itself: one table, every handler a row.
 *
 * @author  Charliechen114514
 * @date    2026-10-07
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_syscall
 * @copyright Copyright (c) 2026
 */

#include <array>
#include <utility>

#include "kernel/proc/signals.hpp"
#include "kernel/syscall/sys_arch_prctl.hpp"
#include "kernel/syscall/sys_brk.hpp"
#include "kernel/syscall/sys_chdir.hpp"
#include "kernel/syscall/sys_clone.hpp"
#include "kernel/syscall/sys_close.hpp"
#include "kernel/syscall/sys_exec.hpp"
#include "kernel/syscall/sys_exit.hpp"
#include "kernel/syscall/sys_fork.hpp"
#include "kernel/syscall/sys_futex.hpp"
#include "kernel/syscall/sys_getcwd.hpp"
#include "kernel/syscall/sys_getdents.hpp"
#include "kernel/syscall/sys_getpid.hpp"
#include "kernel/syscall/sys_getppid.hpp"
#include "kernel/syscall/sys_gettid.hpp"
#include "kernel/syscall/sys_kill.hpp"
#include "kernel/syscall/sys_mkdir.hpp"
#include "kernel/syscall/sys_mmap.hpp"
#include "kernel/syscall/sys_mprotect.hpp"
#include "kernel/syscall/sys_munmap.hpp"
#include "kernel/syscall/sys_open.hpp"
#include "kernel/syscall/sys_pgrp.hpp"
#include "kernel/syscall/sys_read.hpp"
#include "kernel/syscall/sys_reboot.hpp"
#include "kernel/syscall/sys_set_tid_address.hpp"
#include "kernel/syscall/sys_signal.hpp"
#include "kernel/syscall/sys_stat.hpp"
#include "kernel/syscall/sys_unlink.hpp"
#include "kernel/syscall/sys_wait.hpp"
#include "kernel/syscall/sys_write.hpp"
#include "kernel/syscall/sys_yield.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"

namespace {

struct Row {
    cinux::syscall::SyscallNr number;
    cinux::syscall::Handler   handler;
};

constexpr Row kRows[] = {
    {.number = cinux::syscall::SyscallNr::kRead, .handler = cinux::syscall::HandleRead},
    {.number = cinux::syscall::SyscallNr::kWrite, .handler = cinux::syscall::HandleWrite},
    {.number = cinux::syscall::SyscallNr::kOpen, .handler = cinux::syscall::HandleOpen},
    {.number = cinux::syscall::SyscallNr::kClose, .handler = cinux::syscall::HandleClose},
    {.number = cinux::syscall::SyscallNr::kStat, .handler = cinux::syscall::HandleStat},
    {.number = cinux::syscall::SyscallNr::kMmap, .handler = cinux::syscall::HandleMmap},
    {.number = cinux::syscall::SyscallNr::kMunmap, .handler = cinux::syscall::HandleMunmap},
    {.number = cinux::syscall::SyscallNr::kMprotect, .handler = cinux::syscall::HandleMprotect},
    {.number = cinux::syscall::SyscallNr::kBrk, .handler = cinux::syscall::HandleBrk},
    {
        .number  = cinux::syscall::SyscallNr::kRtSigaction,
        .handler = cinux::syscall::HandleRtSigaction,
    },
    {
        .number  = cinux::syscall::SyscallNr::kRtSigprocmask,
        .handler = cinux::syscall::HandleRtSigprocmask,
    },
    {
        .number  = cinux::syscall::SyscallNr::kRtSigreturn,
        .handler = cinux::syscall::HandleRtSigreturn,
    },
    {.number = cinux::syscall::SyscallNr::kYield, .handler = cinux::syscall::HandleYield},
    {.number = cinux::syscall::SyscallNr::kGetpid, .handler = cinux::syscall::HandleGetpid},
    {.number = cinux::syscall::SyscallNr::kClone, .handler = cinux::syscall::HandleClone},
    {.number = cinux::syscall::SyscallNr::kFork, .handler = cinux::syscall::HandleFork},
    {.number = cinux::syscall::SyscallNr::kExecve, .handler = cinux::syscall::HandleExecve},
    {.number = cinux::syscall::SyscallNr::kExit, .handler = cinux::syscall::HandleExit},
    {.number = cinux::syscall::SyscallNr::kWait4, .handler = cinux::syscall::HandleWait4},
    {.number = cinux::syscall::SyscallNr::kKill, .handler = cinux::syscall::HandleKill},
    {.number = cinux::syscall::SyscallNr::kSetpgid, .handler = cinux::syscall::HandleSetpgid},
    {.number = cinux::syscall::SyscallNr::kSetsid, .handler = cinux::syscall::HandleSetsid},
    {.number = cinux::syscall::SyscallNr::kGetpgid, .handler = cinux::syscall::HandleGetpgid},
    {.number = cinux::syscall::SyscallNr::kGetsid, .handler = cinux::syscall::HandleGetsid},
    {.number = cinux::syscall::SyscallNr::kGetppid, .handler = cinux::syscall::HandleGetppid},
    {.number = cinux::syscall::SyscallNr::kGetcwd, .handler = cinux::syscall::HandleGetcwd},
    {.number = cinux::syscall::SyscallNr::kChdir, .handler = cinux::syscall::HandleChdir},
    {.number = cinux::syscall::SyscallNr::kReboot, .handler = cinux::syscall::HandleReboot},
    {.number = cinux::syscall::SyscallNr::kMkdir, .handler = cinux::syscall::HandleMkdir},
    {.number = cinux::syscall::SyscallNr::kUnlink, .handler = cinux::syscall::HandleUnlink},
    {.number = cinux::syscall::SyscallNr::kGetdents, .handler = cinux::syscall::HandleGetdents},
    {
        .number  = cinux::syscall::SyscallNr::kArchPrctl,
        .handler = cinux::syscall::HandleArchPrctl,
    },
    {.number = cinux::syscall::SyscallNr::kGettid, .handler = cinux::syscall::HandleGettid},
    {.number = cinux::syscall::SyscallNr::kFutex, .handler = cinux::syscall::HandleFutex},
    {
        .number  = cinux::syscall::SyscallNr::kSetTidAddress,
        .handler = cinux::syscall::HandleSetTidAddress,
    },
};

constexpr auto kHandlers = [] {
    std::array<cinux::syscall::Handler, cinux::syscall::kSyscallCount> table{};
    for (const Row& row : kRows) {
        table[std::to_underlying(row.number)] = row.handler;
    }
    return table;
}();

}  // namespace

extern "C" long long InvokeSyscall(cinux::syscall::SyscallFrame& frame) {
    const unsigned long long kNumber = frame.rax;
    if (kNumber >= cinux::syscall::kSyscallCount || kHandlers[kNumber] == nullptr) {
        return -cinux::syscall::kEnosys;
    }
    const long long kResult = kHandlers[kNumber](frame);
    frame.rax               = static_cast<unsigned long long>(kResult);
    if (cinux::proc::DeliverPendingSignals(frame)) {
        return static_cast<long long>(frame.rax);
    }
    return kResult;
}
