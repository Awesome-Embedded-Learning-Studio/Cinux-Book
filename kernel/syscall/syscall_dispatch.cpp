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

#include "kernel/boot/print.hpp"
#include "kernel/proc/signals.hpp"
#include "kernel/syscall/sys_access.hpp"
#include "kernel/syscall/sys_arch_prctl.hpp"
#include "kernel/syscall/sys_brk.hpp"
#include "kernel/syscall/sys_chdir.hpp"
#include "kernel/syscall/sys_clock.hpp"
#include "kernel/syscall/sys_clone.hpp"
#include "kernel/syscall/sys_close.hpp"
#include "kernel/syscall/sys_exec.hpp"
#include "kernel/syscall/sys_exit.hpp"
#include "kernel/syscall/sys_fcntl.hpp"
#include "kernel/syscall/sys_fork.hpp"
#include "kernel/syscall/sys_fstat.hpp"
#include "kernel/syscall/sys_futex.hpp"
#include "kernel/syscall/sys_getcwd.hpp"
#include "kernel/syscall/sys_getdents.hpp"
#include "kernel/syscall/sys_getpid.hpp"
#include "kernel/syscall/sys_getppid.hpp"
#include "kernel/syscall/sys_getrandom.hpp"
#include "kernel/syscall/sys_gettid.hpp"
#include "kernel/syscall/sys_id.hpp"
#include "kernel/syscall/sys_ioctl.hpp"
#include "kernel/syscall/sys_itimer.hpp"
#include "kernel/syscall/sys_kill.hpp"
#include "kernel/syscall/sys_mkdir.hpp"
#include "kernel/syscall/sys_mmap.hpp"
#include "kernel/syscall/sys_mprotect.hpp"
#include "kernel/syscall/sys_munmap.hpp"
#include "kernel/syscall/sys_nanosleep.hpp"
#include "kernel/syscall/sys_open.hpp"
#include "kernel/syscall/sys_openat.hpp"
#include "kernel/syscall/sys_pgrp.hpp"
#include "kernel/syscall/sys_pipe.hpp"
#include "kernel/syscall/sys_ppoll.hpp"
#include "kernel/syscall/sys_prlimit.hpp"
#include "kernel/syscall/sys_read.hpp"
#include "kernel/syscall/sys_readlink.hpp"
#include "kernel/syscall/sys_reboot.hpp"
#include "kernel/syscall/sys_set_tid_address.hpp"
#include "kernel/syscall/sys_signal.hpp"
#include "kernel/syscall/sys_socket.hpp"
#include "kernel/syscall/sys_stat.hpp"
#include "kernel/syscall/sys_sysinfo.hpp"
#include "kernel/syscall/sys_tgkill.hpp"
#include "kernel/syscall/sys_uname.hpp"
#include "kernel/syscall/sys_unlink.hpp"
#include "kernel/syscall/sys_wait.hpp"
#include "kernel/syscall/sys_write.hpp"
#include "kernel/syscall/sys_writev.hpp"
#include "kernel/syscall/sys_yield.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"

namespace {

struct Row {
    cinux::syscall::SyscallNr number;
    cinux::syscall::Handler   handler;
};

constexpr Row kRows[] = {
    {.number = cinux::syscall::SyscallNr::kGetitimer, .handler = cinux::syscall::HandleGetitimer},
    {.number = cinux::syscall::SyscallNr::kAlarm, .handler = cinux::syscall::HandleAlarm},
    {.number = cinux::syscall::SyscallNr::kSetitimer, .handler = cinux::syscall::HandleSetitimer},
    {.number = cinux::syscall::SyscallNr::kRead, .handler = cinux::syscall::HandleRead},
    {.number = cinux::syscall::SyscallNr::kWrite, .handler = cinux::syscall::HandleWrite},
    {.number = cinux::syscall::SyscallNr::kOpen, .handler = cinux::syscall::HandleOpen},
    {.number = cinux::syscall::SyscallNr::kClose, .handler = cinux::syscall::HandleClose},
    {.number = cinux::syscall::SyscallNr::kStat, .handler = cinux::syscall::HandleStat},
    {.number = cinux::syscall::SyscallNr::kLstat, .handler = cinux::syscall::HandleStat},
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
    {.number = cinux::syscall::SyscallNr::kVfork, .handler = cinux::syscall::HandleFork},
    {.number = cinux::syscall::SyscallNr::kExecve, .handler = cinux::syscall::HandleExecve},
    {.number = cinux::syscall::SyscallNr::kExit, .handler = cinux::syscall::HandleExit},
    {.number = cinux::syscall::SyscallNr::kExitGroup, .handler = cinux::syscall::HandleExit},
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
    {.number = cinux::syscall::SyscallNr::kFstat, .handler = cinux::syscall::HandleFstat},
    {.number = cinux::syscall::SyscallNr::kIoctl, .handler = cinux::syscall::HandleIoctl},
    {.number = cinux::syscall::SyscallNr::kWritev, .handler = cinux::syscall::HandleWritev},
    {.number = cinux::syscall::SyscallNr::kAccess, .handler = cinux::syscall::HandleAccess},
    {.number = cinux::syscall::SyscallNr::kPipe, .handler = cinux::syscall::HandlePipe},
    {.number = cinux::syscall::SyscallNr::kDup, .handler = cinux::syscall::HandleDup},
    {.number = cinux::syscall::SyscallNr::kDup2, .handler = cinux::syscall::HandleDup2},
    {.number = cinux::syscall::SyscallNr::kPipe2, .handler = cinux::syscall::HandlePipe2},
    {.number = cinux::syscall::SyscallNr::kUname, .handler = cinux::syscall::HandleUname},
    {.number = cinux::syscall::SyscallNr::kFcntl, .handler = cinux::syscall::HandleFcntl},
    {
        .number  = cinux::syscall::SyscallNr::kGettimeofday,
        .handler = cinux::syscall::HandleGettimeofday,
    },
    {.number = cinux::syscall::SyscallNr::kGetuid, .handler = cinux::syscall::HandleGetuid},
    {.number = cinux::syscall::SyscallNr::kGetgid, .handler = cinux::syscall::HandleGetgid},
    {.number = cinux::syscall::SyscallNr::kSetuid, .handler = cinux::syscall::HandleSetuid},
    {.number = cinux::syscall::SyscallNr::kSetgid, .handler = cinux::syscall::HandleSetgid},
    {.number = cinux::syscall::SyscallNr::kGeteuid, .handler = cinux::syscall::HandleGeteuid},
    {.number = cinux::syscall::SyscallNr::kGetegid, .handler = cinux::syscall::HandleGetegid},
    {.number = cinux::syscall::SyscallNr::kPrctl, .handler = cinux::syscall::HandlePrctl},
    {.number = cinux::syscall::SyscallNr::kTime, .handler = cinux::syscall::HandleTime},
    {
        .number  = cinux::syscall::SyscallNr::kClockGettime,
        .handler = cinux::syscall::HandleClockGettime,
    },
    {.number = cinux::syscall::SyscallNr::kPrlimit64, .handler = cinux::syscall::HandlePrlimit64},
    {.number = cinux::syscall::SyscallNr::kGetrandom, .handler = cinux::syscall::HandleGetrandom},
    {.number = cinux::syscall::SyscallNr::kOpenat, .handler = cinux::syscall::HandleOpenat},
    {
        .number  = cinux::syscall::SyscallNr::kNewfstatat,
        .handler = cinux::syscall::HandleNewfstatat,
    },
    {.number = cinux::syscall::SyscallNr::kPpoll, .handler = cinux::syscall::HandlePpoll},
    {.number = cinux::syscall::SyscallNr::kPoll, .handler = cinux::syscall::HandlePoll},
    {.number = cinux::syscall::SyscallNr::kReadlink, .handler = cinux::syscall::HandleReadlink},
    {.number = cinux::syscall::SyscallNr::kSysinfo, .handler = cinux::syscall::HandleSysinfo},
    {.number = cinux::syscall::SyscallNr::kNanosleep, .handler = cinux::syscall::HandleNanosleep},
    {.number = cinux::syscall::SyscallNr::kTgkill, .handler = cinux::syscall::HandleTgkill},
    {.number = cinux::syscall::SyscallNr::kSocket, .handler = cinux::syscall::HandleSocket},
    {.number = cinux::syscall::SyscallNr::kConnect, .handler = cinux::syscall::HandleConnect},
    {.number = cinux::syscall::SyscallNr::kAccept, .handler = cinux::syscall::HandleAccept},
    {.number = cinux::syscall::SyscallNr::kSendto, .handler = cinux::syscall::HandleSendto},
    {.number = cinux::syscall::SyscallNr::kRecvfrom, .handler = cinux::syscall::HandleRecvfrom},
    {.number = cinux::syscall::SyscallNr::kShutdown, .handler = cinux::syscall::HandleShutdown},
    {.number = cinux::syscall::SyscallNr::kBind, .handler = cinux::syscall::HandleBind},
    {.number = cinux::syscall::SyscallNr::kListen, .handler = cinux::syscall::HandleListen},
    {
        .number  = cinux::syscall::SyscallNr::kGetsockname,
        .handler = cinux::syscall::HandleGetsockname,
    },
    {
        .number  = cinux::syscall::SyscallNr::kGetpeername,
        .handler = cinux::syscall::HandleGetpeername,
    },
    {.number = cinux::syscall::SyscallNr::kSocketpair, .handler = cinux::syscall::HandleSocketpair},
    {.number = cinux::syscall::SyscallNr::kSetsockopt, .handler = cinux::syscall::HandleSetsockopt},
    {.number = cinux::syscall::SyscallNr::kGetsockopt, .handler = cinux::syscall::HandleGetsockopt},
    {.number = cinux::syscall::SyscallNr::kAccept4, .handler = cinux::syscall::HandleAccept4},
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
        cinux::print::Println("[kern] syscall %llu unserved", kNumber);
        return -cinux::syscall::kEnosys;
    }
    const long long kResult = kHandlers[kNumber](frame);
    frame.rax               = static_cast<unsigned long long>(kResult);
    if (cinux::proc::DeliverPendingSignals(frame)) {
        return static_cast<long long>(frame.rax);
    }
    return kResult;
}
