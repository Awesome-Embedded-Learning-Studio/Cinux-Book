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

#include "kernel/syscall/sys_close.hpp"
#include "kernel/syscall/sys_exit.hpp"
#include "kernel/syscall/sys_getdents.hpp"
#include "kernel/syscall/sys_mkdir.hpp"
#include "kernel/syscall/sys_open.hpp"
#include "kernel/syscall/sys_read.hpp"
#include "kernel/syscall/sys_unlink.hpp"
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
    {.number = cinux::syscall::SyscallNr::kYield, .handler = cinux::syscall::HandleYield},
    {.number = cinux::syscall::SyscallNr::kExit, .handler = cinux::syscall::HandleExit},
    {.number = cinux::syscall::SyscallNr::kMkdir, .handler = cinux::syscall::HandleMkdir},
    {.number = cinux::syscall::SyscallNr::kUnlink, .handler = cinux::syscall::HandleUnlink},
    {.number = cinux::syscall::SyscallNr::kGetdents, .handler = cinux::syscall::HandleGetdents},
};

constexpr auto kHandlers = [] {
    std::array<cinux::syscall::Handler, cinux::syscall::kSyscallCount> table{};
    for (const Row& row : kRows) {
        table[std::to_underlying(row.number)] = row.handler;
    }
    return table;
}();

}  // namespace

long long InvokeSyscall(unsigned long long number, unsigned long long arg1, unsigned long long arg2,
                        unsigned long long arg3) {
    if (number >= cinux::syscall::kSyscallCount || kHandlers[number] == nullptr) {
        return -cinux::syscall::kEnosys;
    }
    return kHandlers[number](arg1, arg2, arg3);
}
